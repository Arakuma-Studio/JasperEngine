// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animator's MCP tools (docs/specs/S21-animation.md D17, group "animation"):
//   get_animator             a controller (states, transitions, trees, diagnostics), or an
//                            Animator's settings, rig and, while it runs, its live state
//   edit_animator_controller ops on a .controller (parameters, layers, states, sub-state
//                            machines, transitions, default states, blend trees), one undo step
//   control_animator         parameters, triggers, play / crossfade, montages, IK targets and
//                            layer weights of a running Animator (play mode, or the edit preview)
//   edit_avatar_mask         body parts and per-path weights of a .mask
//   edit_montage             clip, slot, rate, blends, sections and events of a .montage
//   sample_animation_pose    bone world transforms of an evaluated pose (state, parameters, time)
// The Animator component itself is edited with add_component / set_properties (S08 tables).

#include "McpTools.h"
#include "W4Shared.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "Project.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimatorAssetDocument.h"
#include "animation/AnimatorAssetUsers.h"
#include "animation/AnimationClipDocument.h"
#include "animation/AvatarMaskWeights.h"
#include "AnimatorTools.h"
#include "mcp/McpValues.h"
#include "window/InspectorAnimLayers.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/ClipBinding.h"
#include "animation/AnimatorInstance.h"
#include "animation/CompiledController.h"
#include "animation/ModelSubClips.h"
#include "animation/PreviewOwner.h"
#include "component/AnimationPlayerComponent.h"
#include "component/AnimatorComponent.h"
#include "component/Transform.h"
#include "animation/AnimatorDebug.h"
#include "object/Animator.h"
#include "pool/AnimatorAssetPool.h"
#include "pose/HumanBones.h"
#include "subsystem/AnimatorSystem.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/Util.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

namespace jasperengine::editor::mcp {

namespace {

namespace fs = std::filesystem;
using namespace ::jasperengine::anim;

Json obj(const Json& properties, const std::vector<std::string>& required = {}) {
    Json out = {{"type", "object"}, {"properties", properties}, {"additionalProperties", false}};
    if (!required.empty()) out["required"] = required;
    return out;
}

Json warningsJson(const std::vector<ClipWarning>& warnings) {
    Json out = Json::array();
    for (const ClipWarning& w : warnings) out.push_back({{"path", w.path}, {"message", w.message}});
    return out;
}

// ------------------------------------------------------------------ paths

// An asset path (project- or assets-relative) of one of the Animator's files, assets-relative
bool assetFilePath(Project* project, const std::string& value, const char* what, bool (*isKind)(const std::string&),
                   std::string& out, fs::path& absolute, std::string& error) {
    if (value.empty()) {
        error = std::string("path is required: a ") + what + " file (project-relative, e.g. \"assets/anim/hero" +
                (isKind == &Util::isAnimatorControllerFile ? ".controller" : (isKind == &Util::isAvatarMaskFile ? ".mask" : ".montage")) + "\").";
        return false;
    }
    if (!isKind(value)) {
        error = "\"" + value + "\" is not a " + what + " file.";
        return false;
    }
    if (!McpRefs::resolveProjectPath(project, value, absolute, error, false)) return false;
    if (!project->isInsideAssetsPath(absolute.string())) absolute = project->resolveAssetPath(value);
    if (!project->isInsideAssetsPath(absolute.string())) {
        error = "\"" + value + "\" is outside the assets folder.";
        return false;
    }
    out = project->normalizeToAssetsRelative(absolute).generic_string();
    return true;
}

// A clip reference from an argument: an .anim (project- or assets-relative) with its GUID, or a
// model sub-clip "<model>#<clip>"; the stored form is assets-relative
bool clipRef(Project* project, const Json& value, AnimatorAssetRef& out, std::string& error) {
    if (!value.is_string() || value.get<std::string>().empty()) {
        error = "a clip is an .anim path or a model clip \"assets/models/hero.gltf#Walk\"";
        return false;
    }
    const std::string text = value.get<std::string>();
    SubClipReference subClip;
    const bool isSubClip = parseSubClipReference(text, subClip);
    const std::string file = isSubClip ? subClip.model : text;
    fs::path absolute;
    if (!McpRefs::resolveProjectPath(project, file, absolute, error, false)) return false;
    if (!project->isInsideAssetsPath(absolute.string())) absolute = project->resolveAssetPath(file);
    std::error_code ec;
    if (!fs::is_regular_file(absolute, ec)) {
        error = "no file \"" + file + "\"";
        return false;
    }
    const std::string stored = project->normalizeToAssetsRelative(absolute).generic_string();
    if (isSubClip) {
        if (!Util::isModelFile(stored)) {
            error = "\"" + file + "\" is not a model (.gltf, .glb, .fbx)";
            return false;
        }
        std::vector<ModelClipInfo> clips;
        std::string listError;
        if (!listModelFileClips(stored, clips, &listError)) {
            error = listError;
            return false;
        }
        bool found = false;
        std::string names;
        for (const ModelClipInfo& clip : clips) {
            found = found || clip.name == subClip.clip || std::to_string(clip.index) == subClip.clip;
            names += (names.empty() ? "" : ", ") + clip.name;
        }
        if (!found) {
            error = "\"" + file + "\" has no animation \"" + subClip.clip + "\" (it has: " + names + ")";
            return false;
        }
        out.path = formatSubClipReference(stored, subClip.clip);
        out.guid.clear();
        return true;
    }
    if (!Util::isAnimationClipFile(stored)) {
        error = "\"" + text + "\" is not an .anim clip or a model clip";
        return false;
    }
    std::string fileText;
    out.path = stored;
    out.guid = ProjectChecks::readFile(absolute, fileText) ? readClipGuid(fileText) : std::string();
    return true;
}

// ------------------------------------------------------------------ controller addressing

LayerDef* findLayerArg(ControllerDef& def, const Json& value, std::string& error) {
    if (value.is_number_integer()) {
        const int index = value.get<int>();
        if (index < 0 || (size_t)index >= def.layers.size()) {
            error = "layer " + std::to_string(index) + " out of range (0.." + std::to_string((int)def.layers.size() - 1) + ")";
            return nullptr;
        }
        return &def.layers[(size_t)index];
    }
    if (value.is_null()) {
        if (def.layers.empty()) {
            error = "the controller has no layer (add_layer)";
            return nullptr;
        }
        return &def.layers[0];
    }
    if (value.is_string()) {
        const int index = def.findLayer(value.get<std::string>());
        if (index >= 0) return &def.layers[(size_t)index];
    }
    std::string names;
    for (const LayerDef& layer : def.layers) names += (names.empty() ? "" : ", ") + layer.name;
    error = "no layer " + value.dump() + " (layers: " + names + ")";
    return nullptr;
}

// "Combat/Air" under a layer's root machine
StateMachineDef* findMachine(StateMachineDef& root, const std::string& path, std::string& error) {
    StateMachineDef* current = &root;
    size_t start = 0;
    while (start < path.size()) {
        const size_t slash = path.find('/', start);
        const std::string segment = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        StateMachineDef* next = nullptr;
        for (StateMachineDef& sub : current->subMachines) {
            if (sub.name == segment) next = &sub;
        }
        if (!next) {
            error = "no sub-state machine \"" + segment + "\" in \"" + (current->name.empty() ? std::string("the root machine") : current->name) + "\"";
            return nullptr;
        }
        current = next;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return current;
}

struct StateLocation {
    LayerDef* layer = nullptr;
    StateMachineDef* machine = nullptr;
    StateDef* state = nullptr;
    size_t index = 0;
};

bool locateIn(StateMachineDef& machine, const std::vector<std::string>& segments, size_t at, StateLocation& out) {
    if (at + 1 == segments.size()) {
        for (size_t i = 0; i < machine.states.size(); i++) {
            if (machine.states[i].name == segments[at]) {
                out.machine = &machine;
                out.state = &machine.states[i];
                out.index = i;
                return true;
            }
        }
        return false;
    }
    for (StateMachineDef& sub : machine.subMachines) {
        if (sub.name == segments[at]) return locateIn(sub, segments, at + 1, out);
    }
    return false;
}

void collectStates(StateMachineDef& machine, const std::string& name, LayerDef* layer, std::vector<StateLocation>& out) {
    for (size_t i = 0; i < machine.states.size(); i++) {
        if (machine.states[i].name == name) out.push_back({layer, &machine, &machine.states[i], i});
    }
    for (StateMachineDef& sub : machine.subMachines) collectStates(sub, name, layer, out);
}

// "Base Layer.Combat.Attack" (layer, machines, state) or a state name unique in the controller
bool findState(ControllerDef& def, const std::string& path, StateLocation& out, std::string& error) {
    std::vector<std::string> segments;
    size_t start = 0;
    while (true) {
        const size_t dot = path.find('.', start);
        segments.push_back(path.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    if (segments.size() >= 2) {
        const int layer = def.findLayer(segments[0]);
        if (layer >= 0){
            out.layer = &def.layers[(size_t)layer];
            if (locateIn(out.layer->machine, segments, 1, out)) return true;
        }
    }
    std::vector<StateLocation> found;
    for (LayerDef& layer : def.layers) collectStates(layer.machine, path, &layer, found);
    if (found.size() == 1) {
        out = found[0];
        return true;
    }
    error = found.empty() ? "no state \"" + path + "\" (name it \"Layer.Machine.State\" or by its unique name)"
                          : "\"" + path + "\" names " + std::to_string(found.size()) + " states: give \"Layer.Machine.State\"";
    return false;
}

// ------------------------------------------------------------------ JSON <-> definitions

bool readCondition(const ControllerDef& def, const Json& value, ConditionDef& out, std::string& error) {
    if (!value.is_object() || !value.contains("parameter") || !value["parameter"].is_string()) {
        error = "a condition is {parameter, mode, threshold?}";
        return false;
    }
    out.parameter = value["parameter"].get<std::string>();
    const int p = def.findParameter(out.parameter);
    if (p < 0) {
        error = "condition: no parameter \"" + out.parameter + "\" (add_parameter first)";
        return false;
    }
    const AnimatorParameterType type = def.parameters[(size_t)p].type;
    std::string mode = value.value("mode", std::string(type == AnimatorParameterType::Float ? "greater" :
                                                       (type == AnimatorParameterType::Int ? "equals" : "if")));
    if (!conditionModeFromName(mode, out.mode)) {
        error = "condition mode \"" + mode + "\": one of if, if_not, greater, less, equals, not_equal";
        return false;
    }
    out.threshold = value.value("threshold", 0.0f);
    return true;
}

bool readTransition(const ControllerDef& def, const Json& value, TransitionDef& t, std::string& error) {
    if (value.contains("to")) t.to = value["to"].get<std::string>();
    if (value.contains("conditions")) {
        t.conditions.clear();
        for (const Json& c : value["conditions"]) {
            ConditionDef condition;
            if (!readCondition(def, c, condition, error)) return false;
            t.conditions.push_back(condition);
        }
    }
    if (value.contains("has_exit_time")) t.hasExitTime = value["has_exit_time"].get<bool>();
    if (value.contains("exit_time")) t.exitTime = value["exit_time"].get<float>();
    if (value.contains("fixed_duration")) t.fixedDuration = value["fixed_duration"].get<bool>();
    if (value.contains("duration")) t.duration = value["duration"].get<float>();
    if (value.contains("offset")) t.offset = value["offset"].get<float>();
    if (value.contains("interruption_source")) {
        const std::string source = value["interruption_source"].get<std::string>();
        if (!interruptionSourceFromName(source, t.interruptionSource)) {
            error = "interruption_source \"" + source + "\": none, current, next, current_then_next or next_then_current";
            return false;
        }
    }
    if (value.contains("ordered_interruption")) t.orderedInterruption = value["ordered_interruption"].get<bool>();
    if (value.contains("can_transition_to_self")) t.canTransitionToSelf = value["can_transition_to_self"].get<bool>();
    if (value.contains("mute")) t.mute = value["mute"].get<bool>();
    return true;
}

bool readMotion(Project* project, const ControllerDef& def, const Json& value, MotionDef& out, std::string& error, int depth);

bool readTree(Project* project, const ControllerDef& def, const Json& value, BlendTreeDef& tree, std::string& error, int depth) {
    if (depth > 8) {
        error = "blend trees nest at most 8 deep here";
        return false;
    }
    const std::string type = value.value("type", std::string("simple_1d"));
    if (!blendTreeTypeFromName(type, tree.type)) {
        error = "blend tree type \"" + type + "\": simple_1d, simple_directional, freeform_directional, freeform_cartesian or direct";
        return false;
    }
    tree.parameter = value.value("parameter", std::string());
    tree.parameterY = value.value("parameter_y", std::string());
    for (const std::string& p : {tree.parameter, tree.parameterY}) {
        if (!p.empty() && def.findParameter(p) < 0) {
            error = "blend tree: no parameter \"" + p + "\"";
            return false;
        }
    }
    tree.children.clear();
    if (value.contains("children")) {
        if (value["children"].size() > MAX_BLEND_CHILDREN) {
            error = "a blend tree has at most " + std::to_string(MAX_BLEND_CHILDREN) + " children";
            return false;
        }
        for (const Json& c : value["children"]) {
            BlendChildDef child;
            if (c.contains("clip") || c.contains("blend_tree")) {
                if (!readMotion(project, def, c, child.motion, error, depth + 1)) return false;
            }
            child.threshold = c.value("threshold", 0.0f);
            if (c.contains("position") && c["position"].is_array() && c["position"].size() == 2) {
                child.position = Vector2(c["position"][0].get<float>(), c["position"][1].get<float>());
            }
            child.timeScale = c.value("time_scale", 1.0f);
            child.directParameter = c.value("direct_parameter", std::string());
            if (c.contains("mirror")) {
                if (!c["mirror"].is_boolean()) {
                    error = "a child's \"mirror\" must be a boolean";
                    return false;
                }
                child.mirror = c["mirror"].get<bool>();
            }
            tree.children.push_back(std::move(child));
        }
    }
    return true;
}

bool readMotion(Project* project, const ControllerDef& def, const Json& value, MotionDef& out, std::string& error, int depth) {
    out = MotionDef();
    if (value.contains("clip") && !value["clip"].is_null()) return clipRef(project, value["clip"], out.clip, error);
    if (value.contains("blend_tree") && !value["blend_tree"].is_null()) {
        out.tree.emplace_back();
        return readTree(project, def, value["blend_tree"], out.tree.back(), error, depth);
    }
    return true;
}

Json refJson(const AnimatorAssetRef& ref) {
    return ref.empty() ? Json(nullptr) : Json(ref.path);
}

Json motionJson(const MotionDef& motion) {
    if (motion.isTree()) {
        const BlendTreeDef& tree = motion.tree.front();
        Json children = Json::array();
        for (const BlendChildDef& child : tree.children) {
            Json c = {{"motion", motionJson(child.motion)}};
            if (tree.type == BlendTreeType::Simple1D) c["threshold"] = child.threshold;
            else if (tree.type == BlendTreeType::Direct) c["direct_parameter"] = child.directParameter;
            else c["position"] = {child.position.x, child.position.y};
            if (child.timeScale != 1.0f) c["time_scale"] = child.timeScale;
            if (child.mirror) c["mirror"] = true;
            children.push_back(c);
        }
        Json out = {{"type", blendTreeTypeName(tree.type)}, {"parameter", tree.parameter}, {"children", children}};
        if (!tree.parameterY.empty()) out["parameter_y"] = tree.parameterY;
        return Json{{"blend_tree", out}};
    }
    if (!motion.clip.empty()) return Json{{"clip", motion.clip.path}};
    return nullptr;
}

Json transitionsJson(const std::vector<TransitionDef>& list) {
    Json out = Json::array();
    for (const TransitionDef& t : list) {
        Json conditions = Json::array();
        for (const ConditionDef& c : t.conditions) {
            Json condition = {{"parameter", c.parameter}, {"mode", conditionModeName(c.mode)}};
            if (c.mode != AnimatorConditionMode::If && c.mode != AnimatorConditionMode::IfNot) condition["threshold"] = c.threshold;
            conditions.push_back(condition);
        }
        Json item = {{"to", t.to}, {"conditions", conditions}, {"duration", t.duration}};
        if (t.hasExitTime) item["exit_time"] = t.exitTime;
        if (!t.fixedDuration) item["fixed_duration"] = false;
        if (t.offset != 0.0f) item["offset"] = t.offset;
        if (t.interruptionSource != TransitionInterruptionSource::None) item["interruption_source"] = interruptionSourceName(t.interruptionSource);
        if (t.canTransitionToSelf) item["can_transition_to_self"] = true;
        if (t.mute) item["mute"] = true;
        out.push_back(item);
    }
    return out;
}

Json machineJson(const StateMachineDef& machine) {
    Json states = Json::array();
    for (const StateDef& s : machine.states) {
        Json state = {{"name", s.name}, {"motion", motionJson(s.motion)}, {"transitions", transitionsJson(s.transitions)},
                      {"position", {s.position.x, s.position.y}}};
        if (s.speed != 1.0f) state["speed"] = s.speed;
        if (!s.speedParameter.empty()) state["speed_parameter"] = s.speedParameter;
        if (!s.timeParameter.empty()) state["time_parameter"] = s.timeParameter;
        if (s.loop != AnimatorStateLoop::Inherit) state["loop"] = stateLoopName(s.loop);
        if (!s.tag.empty()) state["tag"] = s.tag;
        if (s.cycleOffset != 0.0f) state["cycle_offset"] = s.cycleOffset;
        if (s.mirror) state["mirror"] = true;
        if (s.footIK) state["foot_ik"] = true;
        if (s.retarget) state["retarget"] = true;
        states.push_back(state);
    }
    Json subs = Json::array();
    for (const StateMachineDef& sub : machine.subMachines) {
        Json m = machineJson(sub);
        m["name"] = sub.name;
        m["exit_transitions"] = transitionsJson(sub.exitTransitions);
        subs.push_back(m);
    }
    Json out = {{"default_state", machine.defaultState.empty() && !machine.states.empty() ? machine.states.front().name : machine.defaultState},
                {"states", states}};
    if (!machine.entry.empty()) out["entry"] = transitionsJson(machine.entry);
    if (!machine.anyState.empty()) out["any_state"] = transitionsJson(machine.anyState);
    if (!subs.empty()) out["sub_state_machines"] = subs;
    return out;
}

Json controllerJson(const ControllerDef& def) {
    Json parameters = Json::array();
    for (const ParameterDef& p : def.parameters) {
        Json item = {{"name", p.name}, {"type", parameterTypeName(p.type)}};
        if (p.type == AnimatorParameterType::Bool) item["default"] = p.defaultValue != 0.0f;
        else if (p.type != AnimatorParameterType::Trigger) item["default"] = p.defaultValue;
        parameters.push_back(item);
    }
    Json layers = Json::array();
    for (const LayerDef& layer : def.layers) {
        Json l = {{"name", layer.name}, {"weight", layer.weight}, {"blending", layerBlendingName(layer.blending)},
                  {"state_machine", machineJson(layer.machine)}};
        if (!layer.mask.empty()) l["mask"] = layer.mask.path;
        if (!layer.slot.empty()) l["slot"] = layer.slot;
        layers.push_back(l);
    }
    return Json{{"parameters", parameters}, {"layers", layers}};
}

// ------------------------------------------------------------------ masks

Json maskJson(Project* project, const AnimatorAssetDocument& document) {
    const AvatarMaskDef& mask = document.mask();
    Json parts = Json::object();
    for (int i = 0; i < AVATAR_BODY_PART_COUNT; i++) {
        if (mask.bodyParts[i]) parts[bodyPartName((AvatarBodyPart)i)] = *mask.bodyParts[i];
    }
    Json paths = Json::array();
    for (const MaskPathDef& entry : mask.paths) paths.push_back({{"path", entry.path}, {"weight", entry.weight}, {"children", entry.children}});
    Json out = {{"path", document.projectPath()}, {"default", mask.defaultWeight}, {"body_parts", parts}, {"root_hierarchy", mask.rootHierarchy},
                {"paths", paths}};
    if (!mask.guid.empty()) out["guid"] = mask.guid;
    if (!mask.previewModel.empty()) out["preview_model"] = McpValues::assetPathToJson(project, mask.previewModel);
    if (document.converted()) out["read_as"] = animatorFileDialectName(document.dialect());
    if (!document.warnings().empty()) out["warnings"] = warningsJson(document.warnings());
    return out;
}

// ------------------------------------------------------------------ Animator targets

struct AnimatorTarget {
    Project* project = nullptr;
    SceneProject* scene = nullptr;
    Entity entity = NULL_ENTITY;
    AnimatorComponent* component = nullptr;
    bool playing = false;
};

bool resolveAnimator(const CallPtr& call, AnimatorTarget& target) {
    const Json& args = call->args();
    target.project = call->ctx().project();
    std::string error;
    target.scene = McpRefs::resolveScene(target.project, args.contains("scene") ? args["scene"] : Json(), error);
    if (!target.scene) {
        call->fail(error);
        return false;
    }
    if (!target.scene->scene) {
        call->fail(McpRefs::notOpenError(target.project, *target.scene));
        return false;
    }
    target.entity = McpRefs::resolveEntity(target.scene, args["entity"], error);
    if (target.entity == NULL_ENTITY) {
        call->fail(error);
        return false;
    }
    target.component = target.scene->scene->findComponent<AnimatorComponent>(target.entity);
    if (!target.component) {
        call->fail("\"" + target.scene->scene->getEntityName(target.entity) + "\" has no Animator (add_component animator, "
                   "then set_properties {animator: {controller}}).");
        return false;
    }
    target.playing = target.scene->playState != ScenePlayState::STOPPED;
    return true;
}

// The edit-mode preview of one Animator (the system is paused in edit mode): owned through
// PreviewOwner so the Animation window and pose tools stop it; its stop puts the scene back
struct PreviewState {
    PreviewOwner::Token token = 0;
    uint32_t sceneId = 0;
    Entity entity = NULL_ENTITY;
};

std::vector<PreviewState>& previews() {
    static std::vector<PreviewState> list;
    return list;
}

bool previewing(uint32_t sceneId, Entity entity) {
    for (const PreviewState& p : previews()) {
        if (p.sceneId == sceneId && p.entity == entity && PreviewOwner::owns(p.token)) return true;
    }
    return false;
}

void stopPreview(Project* project, uint32_t sceneId, Entity entity) {
    auto& list = previews();
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (it->sceneId == sceneId && it->entity == entity) {
            const PreviewOwner::Token token = it->token;
            list.erase(it);
            PreviewOwner::release(token);
            SceneProject* scene = project->getScene(sceneId);
            if (scene && scene->scene) scene->scene->getSystem<AnimatorSystem>()->restorePreview(entity);
            return;
        }
    }
}

bool startPreview(Project* project, SceneProject* scene, Entity entity) {
    if (previewing(scene->id, entity)) return true;
    const uint32_t sceneId = scene->id;
    const PreviewOwner::Token token = PreviewOwner::acquire(sceneId, entity, scene->scene, "MCP Animator preview", [project, sceneId, entity]() {
        auto& list = previews();
        list.erase(std::remove_if(list.begin(), list.end(), [&](const PreviewState& p){ return p.sceneId == sceneId && p.entity == entity; }), list.end());
        SceneProject* s = project->getScene(sceneId);
        if (s && s->scene) s->scene->getSystem<AnimatorSystem>()->restorePreview(entity);
    });
    previews().push_back({token, sceneId, entity});
    return true;
}

Json liveJson(const AnimatorTarget& target, AnimatorInstance& instance) {
    Json layers = Json::array();
    for (size_t l = 0; l < instance.layers.size(); l++) {
        const AnimatorInstance::StateInfo current = instance.currentState((int)l);
        Json layer = {{"name", instance.controller->layers[l].name}, {"weight", instance.getLayerWeight((int)l)},
                      {"state", current.fullPath}, {"normalized_time", current.normalizedTime}, {"length", current.length}};
        if (instance.isInTransition((int)l)) {
            layer["next"] = instance.nextState((int)l).fullPath;
            layer["transition_progress"] = instance.transitionProgress((int)l);
        }
        layers.push_back(layer);
    }
    Json parameters = Json::object();
    for (size_t p = 0; p < instance.controller->parameters.size(); p++) {
        const ParameterDef& def = instance.controller->parameters[p];
        const float value = instance.parameters[p];
        if (def.type == AnimatorParameterType::Bool || def.type == AnimatorParameterType::Trigger) parameters[def.name] = value != 0.0f;
        else if (def.type == AnimatorParameterType::Int) parameters[def.name] = (int)std::lround(value);
        else parameters[def.name] = value;
    }
    Json out = {{"layers", layers}, {"parameters", parameters}};
    if (const AnimatorMontageRun* montage = instance.activeMontage()) {
        out["montage"] = {{"path", montage->path}, {"section", montage->montage->sections[(size_t)montage->section].name},
                          {"time", montage->time}, {"weight", montage->weight}};
    }
    // the debug numbers (S74 R36): makeSnapshot is their one producer, as for the Inspector
    const Transform* root = target.scene->scene->findComponent<Transform>(target.entity);
    const AnimatorDebugSnapshot snapshot = root ? makeSnapshot(instance, *target.component, *root) : AnimatorDebugSnapshot();
    if (instance.output.hasRootMotion) {
        const Vector3 d = instance.output.rootDeltaPosition;
        out["root_motion"] = {{"delta_position", {d.x, d.y, d.z}}, {"delta_yaw", instance.output.rootDeltaYaw}};
        if (snapshot.valid) addAnimatorDebugRootMotionJson(snapshot, out["root_motion"]);
    }
    Json ik = Json::array();
    for (size_t i = 0; i < target.component->ik.size() && i < instance.ik.size(); i++) {
        const AnimatorIKRun& run = instance.ik[i];
        Json item = {{"name", target.component->ik[i].name}, {"type", ikTypeName(target.component->ik[i].type)}};
        if (run.scriptTarget) item["target"] = {run.scriptTargetWorld.x, run.scriptTargetWorld.y, run.scriptTargetWorld.z};
        else if (run.hasTarget) item["target"] = {run.targetWorld.x, run.targetWorld.y, run.targetWorld.z};
        if (target.component->ik[i].type == AnimatorIKType::FootIK) item["grounded"] = run.hit;
        if (target.component->ik[i].type == AnimatorIKType::FootLock) item["locked"] = run.locked;
        item["reached"] = run.reached;
        if (snapshot.valid && i < snapshot.ik.size()) addAnimatorDebugIKJson(snapshot.ik[i], item);
        ik.push_back(item);
    }
    if (!ik.empty()) out["ik"] = ik;
    return out;
}

Json rigJson(const AnimatorInstance& instance) {
    Json out = {{"bones", instance.rig.bones.size()}, {"skins", instance.rig.skins.size()}, {"humanoid", instance.humanoid.valid}};
    if (!instance.humanoid.valid && !instance.humanoid.error.empty()) out["humanoid_error"] = instance.humanoid.error;
    Json clips = Json::array();
    for (const AnimatorClipSlot& slot : instance.clips) {
        Json c = {{"clip", slot.path}, {"loaded", slot.clip != nullptr}};
        if (slot.clip) {
            c["length"] = slot.length();
            c["bound_tracks"] = slot.retargeted ? Json("retargeted") : Json(slot.binding.boundCount);
            if (!slot.retargeted) c["tracks"] = slot.binding.trackCount;
        }
        if (!slot.error.empty()) c["error"] = slot.error;
        clips.push_back(c);
    }
    out["clips"] = clips;
    return out;
}

// ------------------------------------------------------------------ get_animator

void getAnimatorTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error;
    if (args.contains("controller")) {
        std::string stored;
        fs::path absolute;
        if (!assetFilePath(project, args["controller"].get<std::string>(), "an Animator Controller (.controller)", &Util::isAnimatorControllerFile,
                           stored, absolute, error)) {
            call->fail(error);
            return;
        }
        std::shared_ptr<AnimatorAssetDocument> document = AnimatorAssetDocument::open(project, absolute.string(), &error);
        if (!document) {
            call->fail(error + ". Create one with edit_animator_controller {path, create: true}.");
            return;
        }
        Json out = animator_tools::controllerSummary(project, *document);
        call->ok(animator_tools::controllerSummaryText(*document, out), out);
        return;
    }
    if (!args.contains("entity")) {
        call->fail("Give controller (a .controller path) or entity (a GameObject with an Animator).");
        return;
    }
    AnimatorTarget target;
    if (!resolveAnimator(call, target)) return;
    const AnimatorComponent& c = *target.component;
    Json out = {{"entity", McpRefs::entityJson(target.scene, target.entity)}, {"controller", c.controller.path.empty() ? std::string() : target.project->assetHintToProjectPath(c.controller.path, AssetPathBase::Assets)}, {"enabled", c.enabled},
                {"speed", c.speed}, {"apply_root_motion", c.applyRootMotion}, {"playing", target.playing}};
    AnimatorSystem* system = target.scene->scene->getSystem<AnimatorSystem>().get();
    const bool live = target.playing || previewing(target.scene->id, target.entity);
    bool inspected = false;
    if (!live) {
        // edit mode: bind once to report the rig and clips, then put everything back
        std::string bindError;
        inspected = system->preview(target.entity, 0.0f, &bindError);
        if (!inspected) out["bind_error"] = bindError;
    }
    AnimatorInstance* instance = c.runtime.get();
    if (instance && instance->controller && instance->bound) {
        out["rig"] = rigJson(*instance);
        out["diagnostics"] = warningsJson(instance->controller->diagnostics);
        if (live) out["live"] = liveJson(target, *instance);
    }else if (instance && !instance->controllerError.empty()) {
        out["controller_error"] = instance->controllerError;
    }
    if (inspected) system->restorePreview(target.entity);
    std::string text = "Animator on \"" + target.scene->scene->getEntityName(target.entity) + "\" (" +
                       (c.controller.path.empty() ? std::string("no controller") : c.controller.path) + ")";
    if (out.contains("live")) {
        for (const Json& layer : out["live"]["layers"]) text += "; " + layer["name"].get<std::string>() + ": " + layer["state"].get<std::string>();
    }
    call->ok(text + ".", out);
}

// ------------------------------------------------------------------ edit_animator_controller

void renameParameterEverywhere(MotionDef& motion, const std::string& from, const std::string& to) {
    if (!motion.isTree()) return;
    BlendTreeDef& tree = motion.tree.front();
    if (tree.parameter == from) tree.parameter = to;
    if (tree.parameterY == from) tree.parameterY = to;
    for (BlendChildDef& child : tree.children) {
        if (child.directParameter == from) child.directParameter = to;
        renameParameterEverywhere(child.motion, from, to);
    }
}

void renameParameterEverywhere(StateMachineDef& machine, const std::string& from, const std::string& to) {
    auto fix = [&](std::vector<TransitionDef>& list) {
        for (TransitionDef& t : list) for (ConditionDef& c : t.conditions) if (c.parameter == from) c.parameter = to;
    };
    fix(machine.entry);
    fix(machine.anyState);
    fix(machine.exitTransitions);
    for (StateDef& s : machine.states) {
        fix(s.transitions);
        renameParameterEverywhere(s.motion, from, to);
        for (std::string* p : {&s.speedParameter, &s.cycleOffsetParameter, &s.timeParameter}) if (*p == from) *p = to;
    }
    for (StateMachineDef& sub : machine.subMachines) renameParameterEverywhere(sub, from, to);
}

bool applyStateFields(Project* project, const ControllerDef& def, const Json& op, StateDef& s, std::string& error) {
    if (op.contains("motion") || op.contains("clip") || op.contains("blend_tree")) {
        const Json source = op.contains("motion") ? op["motion"] : op;
        if (!readMotion(project, def, source, s.motion, error, 0)) return false;
    }
    if (op.contains("position") && op["position"].is_array() && op["position"].size() == 2) {
        s.position = Vector2(op["position"][0].get<float>(), op["position"][1].get<float>());
    }
    if (op.contains("speed")) s.speed = op["speed"].get<float>();
    for (const auto& [key, member] : std::initializer_list<std::pair<const char*, std::string StateDef::*>>{
            {"speed_parameter", &StateDef::speedParameter}, {"time_parameter", &StateDef::timeParameter},
            {"cycle_offset_parameter", &StateDef::cycleOffsetParameter}}) {
        if (!op.contains(key)) continue;
        const std::string value = op[key].is_null() ? std::string() : op[key].get<std::string>();
        if (!value.empty() && def.findParameter(value) < 0) {
            error = std::string(key) + ": no parameter \"" + value + "\"";
            return false;
        }
        s.*member = value;
    }
    if (op.contains("cycle_offset")) s.cycleOffset = op["cycle_offset"].get<float>();
    if (op.contains("tag")) s.tag = op["tag"].get<std::string>();
    if (op.contains("loop")) {
        const std::string loop = op["loop"].get<std::string>();
        if (!stateLoopFromName(loop, s.loop)) {
            error = "loop \"" + loop + "\": inherit, loop, once, ping_pong, clamp_forever or repeat";
            return false;
        }
    }
    if (op.contains("additive_reference")) {
        const std::string reference = op["additive_reference"].get<std::string>();
        if (!additiveReferenceFromName(reference, s.additiveReference)) {
            error = "additive_reference \"" + reference + "\": first_frame, rest, time or clip";
            return false;
        }
    }
    if (op.contains("additive_time")) s.additiveTime = op["additive_time"].get<float>();
    // humanoid playback (the one retarget path)
    if (op.contains("mirror")) s.mirror = op["mirror"].get<bool>();
    if (op.contains("foot_ik")) s.footIK = op["foot_ik"].get<bool>();
    if (op.contains("retarget")) s.retarget = op["retarget"].get<bool>();
    return true;
}

// the transition list a `from` names: a state path, "Any State" / "Entry" (of `machine` in
// `layer`), or "Exit:<machine path>" (a sub-state machine's exit transitions)
std::vector<TransitionDef>* transitionList(ControllerDef& def, const Json& op, std::string& error) {
    const std::string from = op.value("from", std::string());
    if (from == "Any State" || from == "Entry") {
        LayerDef* layer = findLayerArg(def, op.contains("layer") ? op["layer"] : Json(), error);
        if (!layer) return nullptr;
        StateMachineDef* machine = findMachine(layer->machine, op.value("machine", std::string()), error);
        if (!machine) return nullptr;
        return from == "Entry" ? &machine->entry : &machine->anyState;
    }
    if (from.rfind("Exit:", 0) == 0) {
        LayerDef* layer = findLayerArg(def, op.contains("layer") ? op["layer"] : Json(), error);
        if (!layer) return nullptr;
        StateMachineDef* machine = findMachine(layer->machine, from.substr(5), error);
        if (!machine) return nullptr;
        if (machine == &layer->machine) {
            error = "the root machine has no exit transitions";
            return nullptr;
        }
        return &machine->exitTransitions;
    }
    StateLocation location;
    if (!findState(def, from, location, error)) return nullptr;
    return &location.state->transitions;
}

std::vector<std::string> controllerOps() {
    return {"add_parameter", "remove_parameter", "rename_parameter", "set_parameter", "add_layer", "set_layer", "remove_layer",
            "add_state", "set_state", "remove_state", "add_sub_state_machine", "remove_sub_state_machine", "add_transition",
            "set_transition", "remove_transition", "set_default_state", "set_blend_tree"};
}

// the fields an op may carry and their JSON kinds (the listing keeps ops free-form to stay small:
// their fields are in docs/mcp/guide/animator.md, checked here)
bool checkOpFields(const Json& op, std::string& error) {
    if (!op.is_object()) {
        error = "an op is an object {op, ...}";
        return false;
    }
    enum K { Str, Num, Bool, Arr, Obj, Any };
    static const std::vector<std::pair<const char*, K>> fields = {
        {"op", Str}, {"name", Str}, {"new_name", Str}, {"type", Str}, {"default", Any}, {"layer", Any}, {"machine", Str},
        {"state", Str}, {"weight", Num}, {"blending", Str}, {"mask", Any}, {"slot", Str}, {"clip", Str}, {"blend_tree", Obj},
        {"position", Arr}, {"speed", Num}, {"speed_parameter", Str}, {"time_parameter", Str}, {"cycle_offset", Num},
        {"loop", Str}, {"tag", Str}, {"additive_reference", Str}, {"additive_time", Num}, {"from", Str}, {"to", Str},
        {"index", Num}, {"conditions", Arr}, {"has_exit_time", Bool}, {"exit_time", Num}, {"fixed_duration", Bool},
        {"duration", Num}, {"offset", Num}, {"interruption_source", Str}, {"ordered_interruption", Bool},
        {"can_transition_to_self", Bool}, {"mute", Bool}, {"motion", Obj}, {"mirror", Bool}, {"foot_ik", Bool}, {"retarget", Bool}};
    for (auto it = op.begin(); it != op.end(); ++it) {
        auto found = std::find_if(fields.begin(), fields.end(), [&](const auto& f) { return it.key() == f.first; });
        if (found == fields.end()) {
            error = "unknown field \"" + it.key() + "\" (the ops' fields: jasper://docs/animator)";
            return false;
        }
        const Json& v = it.value();
        const bool ok = found->second == Any || (found->second == Str && v.is_string()) || (found->second == Num && v.is_number()) ||
                        (found->second == Bool && v.is_boolean()) || (found->second == Arr && v.is_array()) ||
                        (found->second == Obj && v.is_object());
        if (!ok) {
            static const char* const kinds[] = {"a string", "a number", "a boolean", "an array", "an object", ""};
            error = "\"" + it.key() + "\" must be " + kinds[found->second];
            return false;
        }
    }
    if (op.contains("default") && !op["default"].is_number() && !op["default"].is_boolean()) {
        error = "\"default\" must be a number or a boolean";
        return false;
    }
    if (op.contains("position") && (op["position"].size() != 2 || !op["position"][0].is_number() || !op["position"][1].is_number())) {
        error = "\"position\" must be [x, y]";
        return false;
    }
    // numbers out of their range are refused (upstream's rule: never silently clamped)
    static const std::vector<std::tuple<const char*, float, float>> ranges = {
        {"weight", 0.0f, 1.0f}, {"offset", 0.0f, 1.0f}, {"cycle_offset", 0.0f, 1.0f}, {"duration", 0.0f, 1e9f}, {"exit_time", 0.0f, 1e9f},
        {"additive_time", 0.0f, 1e9f}};
    for (const auto& [key, lo, hi] : ranges) {
        if (!op.contains(key)) continue;
        const double v = op[key].get<double>();
        if (!std::isfinite(v) || v < lo || v > hi) {
            error = std::string("\"") + key + "\" must be " + (hi > 1e8f ? std::string("0 or more") : std::string("from 0 to 1"));
            return false;
        }
    }
    if (op.contains("speed") && !std::isfinite(op["speed"].get<double>())) {
        error = "\"speed\" must be a finite number";
        return false;
    }
    return true;
}

bool applyOp(Project* project, ControllerDef& def, const Json& op, std::vector<std::string>& changes, std::string& error) {
    if (!checkOpFields(op, error)) return false;
    const std::string kind = op.value("op", std::string());
    const std::string name = op.value("name", std::string());
    if (kind == "add_parameter") {
        if (name.empty() || def.findParameter(name) >= 0) {
            error = name.empty() ? "add_parameter needs a name" : "a parameter \"" + name + "\" exists already";
            return false;
        }
        ParameterDef p;
        p.name = name;
        const std::string type = op.value("type", std::string("float"));
        if (!parameterTypeFromName(type, p.type)) {
            error = "type \"" + type + "\": float, int, bool or trigger";
            return false;
        }
        if (op.contains("default")) p.defaultValue = op["default"].is_boolean() ? (op["default"].get<bool>() ? 1.0f : 0.0f) : op["default"].get<float>();
        if (p.type == AnimatorParameterType::Int && p.defaultValue != std::round(p.defaultValue)) {
            error = "\"default\" of an int parameter must be a whole number";
            return false;
        }
        def.parameters.push_back(p);
        changes.push_back("add_parameter " + name);
        return true;
    }
    if (kind == "remove_parameter" || kind == "rename_parameter" || kind == "set_parameter") {
        const int index = def.findParameter(name);
        if (index < 0) {
            error = "no parameter \"" + name + "\"";
            return false;
        }
        if (kind == "remove_parameter") {
            def.parameters.erase(def.parameters.begin() + index);
            // its conditions go, the trees and state parameters reading it are cleared
            removeParameterReferences(def, name);
        } else if (kind == "rename_parameter") {
            const std::string to = op.value("new_name", std::string());
            if (to.empty() || def.findParameter(to) >= 0) {
                error = to.empty() ? "rename_parameter needs new_name" : "a parameter \"" + to + "\" exists already";
                return false;
            }
            def.parameters[(size_t)index].name = to;
            for (LayerDef& layer : def.layers) renameParameterEverywhere(layer.machine, name, to);
        } else {
            ParameterDef& p = def.parameters[(size_t)index];
            if (op.contains("default")) p.defaultValue = op["default"].is_boolean() ? (op["default"].get<bool>() ? 1.0f : 0.0f) : op["default"].get<float>();
        }
        changes.push_back(kind + " " + name);
        return true;
    }
    if (kind == "add_layer") {
        if (name.empty() || def.findLayer(name) >= 0) {
            error = name.empty() ? "add_layer needs a name" : "a layer \"" + name + "\" exists already";
            return false;
        }
        LayerDef layer;
        layer.name = name;
        def.layers.push_back(layer);
        Json set = op;
        set["layer"] = name;
        set.erase("name");
        set["op"] = "set_layer";
        if (!applyOp(project, def, set, changes, error)) return false;
        changes.back() = "add_layer " + name;
        return true;
    }
    if (kind == "set_layer" || kind == "remove_layer") {
        LayerDef* layer = findLayerArg(def, op.contains("layer") ? op["layer"] : Json(), error);
        if (!layer) return false;
        if (kind == "remove_layer") {
            if (def.layers.size() <= 1) {
                error = "a controller keeps one layer (\"" + layer->name + "\" is its last)";
                return false;
            }
            changes.push_back("remove_layer " + layer->name);
            def.layers.erase(def.layers.begin() + (layer - def.layers.data()));
            return true;
        }
        if (op.contains("new_name")) layer->name = op["new_name"].get<std::string>();
        if (op.contains("weight")) layer->weight = op["weight"].get<float>();
        if (op.contains("blending")) {
            const std::string blending = op["blending"].get<std::string>();
            if (!layerBlendingFromName(blending, layer->blending)) {
                error = "blending \"" + blending + "\": override or additive";
                return false;
            }
        }
        if (op.contains("mask")) {
            layer->mask = AnimatorAssetRef();
            if (!op["mask"].is_null()) {
                std::string stored;
                fs::path absolute;
                if (!assetFilePath(project, op["mask"].get<std::string>(), "an Avatar Mask (.mask)", &Util::isAvatarMaskFile, stored, absolute, error)) return false;
                std::string text;
                layer->mask.path = stored;
                layer->mask.guid = ProjectChecks::readFile(absolute, text) ? readAnimatorAssetGuid(text) : std::string();
            }
        }
        if (op.contains("slot")) layer->slot = op["slot"].get<std::string>();
        changes.push_back("set_layer " + layer->name);
        return true;
    }
    if (kind == "add_state" || kind == "add_sub_state_machine" || kind == "remove_sub_state_machine") {
        LayerDef* layer = findLayerArg(def, op.contains("layer") ? op["layer"] : Json(), error);
        if (!layer) return false;
        StateMachineDef* machine = findMachine(layer->machine, op.value("machine", std::string()), error);
        if (!machine) return false;
        if (kind == "remove_sub_state_machine") {
            for (size_t i = 0; i < machine->subMachines.size(); i++) {
                if (machine->subMachines[i].name == name) {
                    machine->subMachines.erase(machine->subMachines.begin() + (long)i);
                    changes.push_back("remove_sub_state_machine " + name);
                    return true;
                }
            }
            error = "no sub-state machine \"" + name + "\"";
            return false;
        }
        if (name.empty() || name.find('/') != std::string::npos || name.find('.') != std::string::npos || name == "Exit" || name == "Entry" ||
                name == "Any State") {
            error = "a state or machine name is not empty, has no '/' or '.', and is not Exit, Entry or Any State";
            return false;
        }
        for (const StateDef& s : machine->states) if (s.name == name) { error = "\"" + name + "\" exists already"; return false; }
        for (const StateMachineDef& m : machine->subMachines) if (m.name == name) { error = "\"" + name + "\" exists already"; return false; }
        if (kind == "add_sub_state_machine") {
            StateMachineDef sub;
            sub.name = name;
            if (op.contains("position") && op["position"].is_array() && op["position"].size() == 2) {
                sub.position = Vector2(op["position"][0].get<float>(), op["position"][1].get<float>());
            }
            machine->subMachines.push_back(sub);
            changes.push_back("add_sub_state_machine " + name);
            return true;
        }
        StateDef s;
        s.name = name;
        // a grid placement unless given: the editor lays states out left to right
        s.position = Vector2(240.0f + 220.0f * (float)(machine->states.size() % 4), 120.0f + 90.0f * (float)(machine->states.size() / 4));
        if (!applyStateFields(project, def, op, s, error)) return false;
        machine->states.push_back(std::move(s));
        if (op.value("default", false)) machine->defaultState = name;
        changes.push_back("add_state " + name);
        return true;
    }
    if (kind == "set_state" || kind == "remove_state") {
        StateLocation location;
        if (!findState(def, op.value("state", name), location, error)) return false;
        if (kind == "remove_state") {
            const std::string gone = location.state->name;
            changes.push_back("remove_state " + gone);
            StateMachineDef& machine = *location.machine;
            machine.states.erase(machine.states.begin() + (long)location.index);
            // the transitions into it go with it (the Animator window's rule), the default moves on
            auto into = [&](const TransitionDef& t) { return t.to == gone; };
            auto prune = [&](std::vector<TransitionDef>& list) { list.erase(std::remove_if(list.begin(), list.end(), into), list.end()); };
            prune(machine.entry);
            prune(machine.anyState);
            for (StateDef& other : machine.states) prune(other.transitions);
            for (StateMachineDef& sub : machine.subMachines) prune(sub.exitTransitions);
            if (machine.defaultState == gone) machine.defaultState = machine.states.empty() ? std::string() : machine.states.front().name;
            return true;
        }
        if (op.contains("new_name")) location.state->name = op["new_name"].get<std::string>();
        if (!applyStateFields(project, def, op, *location.state, error)) return false;
        changes.push_back("set_state " + location.state->name);
        return true;
    }
    if (kind == "set_default_state") {
        LayerDef* layer = findLayerArg(def, op.contains("layer") ? op["layer"] : Json(), error);
        if (!layer) return false;
        StateMachineDef* machine = findMachine(layer->machine, op.value("machine", std::string()), error);
        if (!machine) return false;
        const std::string state = op.value("state", std::string());
        bool exists = false;
        for (const StateDef& s : machine->states) exists = exists || s.name == state;
        for (const StateMachineDef& m : machine->subMachines) exists = exists || m.name == state;
        if (!exists) {
            error = "no state \"" + state + "\" in that machine";
            return false;
        }
        machine->defaultState = state;
        changes.push_back("set_default_state " + state);
        return true;
    }
    if (kind == "set_blend_tree") {
        StateLocation location;
        if (!findState(def, op.value("state", std::string()), location, error)) return false;
        Json motion = Json::object();
        motion["blend_tree"] = op.contains("blend_tree") ? op["blend_tree"] : Json::object();
        if (!readMotion(project, def, motion, location.state->motion, error, 0)) return false;
        changes.push_back("set_blend_tree " + location.state->name);
        return true;
    }
    if (kind == "add_transition" || kind == "set_transition" || kind == "remove_transition") {
        std::vector<TransitionDef>* list = transitionList(def, op, error);
        if (!list) return false;
        if (kind == "add_transition") {
            TransitionDef t;
            if (!readTransition(def, op, t, error)) return false;
            if (t.to.empty()) {
                error = "add_transition needs to (a state, a sub-state machine, \"Exit\", or \"/State\" from the layer's root)";
                return false;
            }
            list->push_back(std::move(t));
            changes.push_back("add_transition " + op.value("from", std::string()) + " -> " + list->back().to);
            return true;
        }
        const int index = op.value("index", 0);
        if (index < 0 || (size_t)index >= list->size()) {
            error = "transition index " + std::to_string(index) + " out of range (" + std::to_string(list->size()) + " transitions)";
            return false;
        }
        if (kind == "remove_transition") {
            list->erase(list->begin() + index);
        } else if (!readTransition(def, op, (*list)[(size_t)index], error)) {
            return false;
        }
        changes.push_back(kind + " " + op.value("from", std::string()) + "[" + std::to_string(index) + "]");
        return true;
    }
    std::string known;
    for (const std::string& name : controllerOps()) known += (known.empty() ? "" : ", ") + name;
    error = "unknown op \"" + kind + "\" (" + known + ")";
    return false;
}

void editAnimatorControllerTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error, stored;
    fs::path absolute;
    if (!assetFilePath(project, args.value("path", std::string()), "an Animator Controller (.controller)", &Util::isAnimatorControllerFile,
                       stored, absolute, error)) {
        call->fail(error);
        return;
    }
    std::error_code ec;
    const bool exists = fs::exists(absolute, ec);
    ControllerDef def;
    std::shared_ptr<AnimatorAssetDocument> document;
    std::vector<std::string> changes;
    SceneProject* playerScene = nullptr;
    Entity playerEntity = NULL_ENTITY;
    if (args.value("create", false) && !exists) {
        LayerDef layer;
        layer.name = "Base Layer";
        def.layers.push_back(layer);
    } else if (!exists) {
        call->fail("No controller at \"" + args.value("path", std::string()) + "\": pass create: true to make it.");
        return;
    } else {
        document = AnimatorAssetDocument::open(project, absolute.string(), &error);
        if (!document) {
            call->fail(error);
            return;
        }
        def = document->controller();
    }
    // from_player: a state per Animation Player clip (its default the first), the player's entity
    if (args.contains("from_player")) {
        AnimatorTarget target;
        target.project = project;
        target.scene = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
        if (!target.scene || !target.scene->scene) {
            call->fail(target.scene ? McpRefs::notOpenError(project, *target.scene) : error);
            return;
        }
        const Entity entity = McpRefs::resolveEntity(target.scene, args["from_player"], error);
        AnimationPlayerComponent* player = entity != NULL_ENTITY ? target.scene->scene->findComponent<AnimationPlayerComponent>(entity) : nullptr;
        if (!player) {
            call->fail(entity == NULL_ENTITY ? error : "\"" + target.scene->scene->getEntityName(entity) + "\" has no Animation Player");
            return;
        }
        appendPlayerStates(def, *player);
        changes.push_back("from_player: " + std::to_string(player->clips.size()) + " state(s)");
        playerScene = target.scene;
        playerEntity = entity;
    }
    if (args.contains("ops")) {
        // a transition to a state that does not exist is refused (the op names it), not left to
        // the diagnostics: the transitions the file had before are not this call's
        auto missingTargets = [](const ControllerDef& d, std::string* first) {
            size_t n = 0;
            // (held: a range-for over a member of the temporary would read a freed controller)
            const std::shared_ptr<const CompiledController> compiled = compileController(d);
            for (const ClipWarning& w : compiled->diagnostics) {
                if (w.message.rfind("no state or state machine named", 0) != 0) continue;
                if (first && first->empty()) *first = w.message;
                n++;
            }
            return n;
        };
        size_t missing = missingTargets(def, nullptr);
        for (size_t i = 0; i < args["ops"].size(); i++) {
            const Json& op = args["ops"][i];
            if (!applyOp(project, def, op, changes, error)) {
                call->fail("ops[" + std::to_string(i) + "]: " + error + " (nothing was written).");
                return;
            }
            const std::string kind = op.is_object() ? op.value("op", std::string()) : std::string();
            if (kind == "add_transition" || kind == "set_transition") {
                std::string message;
                const size_t now = missingTargets(def, &message);
                if (now > missing) {
                    call->fail("ops[" + std::to_string(i) + "].to: \"" + op.value("to", std::string()) + "\": " + message +
                               " (nothing was written).");
                    return;
                }
                missing = now;
            }
        }
    }
    normalizeController(def);
    bool assigned = false;
    if (!document && playerEntity != NULL_ENTITY && !playerScene->scene->findComponent<AnimatorComponent>(playerEntity)) {
        // a new controller from a player without an Animator (upstream's create.from_entity, the
        // Inspector's Create Animator Controller from Clips): an Animator plays it and the player
        // hands over to it (disabled), one scene undo step with the new file
        const std::string relative = fs::relative(absolute, project->getProjectPath()).generic_string();
        if (!animlayersui::createControllerFile(project, playerScene->id, playerEntity, relative, def, error)) {
            call->fail(error);
            return;
        }
        document = AnimatorAssetDocument::open(project, absolute.string(), &error);
        if (!document) {
            call->fail(error);
            return;
        }
        changes.insert(changes.begin(), "created");
        changes.push_back("an Animator on \"" + playerScene->scene->getEntityName(playerEntity) + "\" plays it; its player hands over (disabled)");
        assigned = true;
    }
    if (!document) {
        document = AnimatorAssetDocument::create(project, absolute.string(), AnimatorAssetDocument::Kind::Controller,
                                                 "Create Animator Controller " + absolute.filename().string(), &error);
        if (!document) {
            call->fail(error);
            return;
        }
        changes.insert(changes.begin(), "created");
    }
    if (!assigned && !document->commit(def, "Edit Animator Controller " + absolute.filename().string(), std::string(), &error)) {
        call->fail(error);
        return;
    }
    std::shared_ptr<const CompiledController> compiled = compileController(document->controller());
    const std::vector<ClipWarning> problems = animator_tools::controllerWarnings(project, document->controller());
    Json out = {{"path", document->projectPath()}, {"changes", changes}, {"diagnostics", warningsJson(compiled->diagnostics)},
                {"warnings", warningsJson(problems)}, {"users", animator_tools::controllerUsers(project, document->projectPath())},
                {"controller", controllerJson(document->controller())}};
    std::string text = document->projectPath() + ": " + std::to_string(changes.size()) + " change(s), " + std::to_string(compiled->diagnostics.size()) +
                       " diagnostic(s), " + std::to_string(problems.size()) + " warning(s); " +
                       (assigned ? "one scene undo step (the file with the Animator)." : "one project undo step.");
    size_t shown = 0;
    for (const ClipWarning& warning : problems) {
        if (shown++ >= 8) break;
        text += "\nwarning: " + warning.path + ": " + warning.message;
    }
    call->ok(text, out);
}

// ------------------------------------------------------------------ control_animator

void controlAnimatorTool(const CallPtr& call) {
    const Json& args = call->args();
    AnimatorTarget target;
    if (!resolveAnimator(call, target)) return;
    AnimatorSystem* system = target.scene->scene->getSystem<AnimatorSystem>().get();
    const std::string preview = args.value("preview", std::string());
    if (!target.playing) {
        if (preview == "stop") {
            stopPreview(target.project, target.scene->id, target.entity);
            call->ok("Preview stopped: the scene is as it was.", Json{{"previewing", false}});
            return;
        }
        if (preview.empty() && !previewing(target.scene->id, target.entity)) {
            call->fail("The scene is not playing: start play mode, or pass preview: \"start\" to evaluate the Animator in edit mode "
                       "(\"stop\" puts the scene back).");
            return;
        }
        startPreview(target.project, target.scene, target.entity);
        std::string error;
        if (!system->preview(target.entity, 0.0f, &error)) {
            stopPreview(target.project, target.scene->id, target.entity);
            call->fail("Cannot preview: " + error);
            return;
        }
    }
    AnimatorInstance* instance = system->instance(target.entity);
    if (!instance || !instance->controller) {
        call->fail("The Animator has no loaded controller" + (instance && !instance->controllerError.empty() ? ": " + instance->controllerError : std::string()));
        return;
    }
    std::vector<std::string> done, errors;
    if (args.contains("parameters")) {
        for (auto it = args["parameters"].begin(); it != args["parameters"].end(); ++it) {
            const Json& v = it.value();
            const float value = v.is_boolean() ? (v.get<bool>() ? 1.0f : 0.0f) : v.get<float>();
            if (instance->setFloat(it.key(), value)) done.push_back(it.key());
            else errors.push_back("no parameter \"" + it.key() + "\"");
        }
    }
    if (args.contains("triggers")) {
        for (const Json& t : args["triggers"]) {
            if (instance->setTrigger(t.get<std::string>())) done.push_back("trigger " + t.get<std::string>());
            else errors.push_back("no parameter \"" + t.get<std::string>() + "\"");
        }
    }
    if (args.contains("reset_triggers")) {
        for (const Json& t : args["reset_triggers"]) instance->resetTrigger(t.get<std::string>());
    }
    if (args.contains("layer_weights")) {
        for (auto it = args["layer_weights"].begin(); it != args["layer_weights"].end(); ++it) {
            const int layer = instance->getLayerIndex(it.key());
            if (layer < 0) errors.push_back("no layer \"" + it.key() + "\"");
            else instance->setLayerWeight(layer, it.value().get<float>());
        }
    }
    if (args.contains("play")) {
        const Json& p = args["play"];
        const std::string state = p.value("state", std::string());
        const float duration = p.value("duration", 0.0f);
        bool ok;
        if (duration > 0.0f) ok = instance->crossFade(state, duration, p.value("fixed_duration", true), p.value("layer", -1), p.value("offset", 0.0f));
        else ok = instance->play(state, p.value("layer", -1), p.value("normalized_time", -1.0f));
        if (ok) done.push_back((duration > 0.0f ? "crossfade " : "play ") + state);
        else errors.push_back("no state \"" + state + "\"");
    }
    if (args.contains("montage")) {
        const Json& m = args["montage"];
        std::string stored, error;
        fs::path absolute;
        if (!assetFilePath(target.project, m.value("path", std::string()), "an Animation Montage (.montage)", &Util::isMontageFile, stored, absolute, error)) {
            errors.push_back(error);
        } else if (!system->playMontage(target.entity, stored, m.value("rate", 1.0f), m.value("section", std::string()), &error)) {
            errors.push_back(error);
        } else {
            done.push_back("montage " + stored);
        }
    }
    if (args.value("stop_montage", false)) {
        instance->stopMontage(nullptr, -1.0f);
        done.push_back("stop_montage");
    }
    if (args.contains("montage_section")) {
        const Json& s = args["montage_section"];
        if (s.contains("jump")) {
            if (instance->montageJumpToSection(s["jump"].get<std::string>())) done.push_back("jump " + s["jump"].get<std::string>());
            else errors.push_back("no playing montage with section \"" + s["jump"].get<std::string>() + "\"");
        }
        if (s.contains("from")) {
            if (instance->montageSetNextSection(s["from"].get<std::string>(), s.value("next", std::string()))) done.push_back("next section");
            else errors.push_back("cannot link section \"" + s["from"].get<std::string>() + "\"");
        }
    }
    if (args.contains("ik")) {
        for (auto it = args["ik"].begin(); it != args["ik"].end(); ++it) {
            size_t index = target.component->ik.size();
            for (size_t i = 0; i < target.component->ik.size(); i++) if (target.component->ik[i].name == it.key()) index = i;
            if (index >= instance->ik.size()) {
                errors.push_back("no IK constraint \"" + it.key() + "\"");
                continue;
            }
            const Json& v = it.value();
            if (v.contains("target")) {
                if (v["target"].is_null()) {
                    instance->ik[index].scriptTarget = false;
                } else {
                    instance->ik[index].scriptTarget = true;
                    instance->ik[index].scriptTargetWorld = Vector3(v["target"][0].get<float>(), v["target"][1].get<float>(), v["target"][2].get<float>());
                }
            }
            if (v.contains("weight")) instance->ik[index].scriptWeight = v["weight"].is_null() ? -1.0f : std::min(std::max(v["weight"].get<float>(), 0.0f), 1.0f);
            if (v.contains("soften") || v.contains("twist") || v.contains("hinge_axis") || v.contains("aim_offset")) {
                // stored settings of the constraint: the game's copy while playing; the scene's
                // only through set_properties, which can undo
                if (!target.playing) {
                    errors.push_back("ik " + it.key() + ": soften, twist, hinge_axis and aim_offset need play mode; in edit mode set them with set_properties (ik[n].soften ...)");
                    continue;
                }
                Animator object(target.scene->scene, target.entity);
                const auto vector = [](const Json& j) { return Vector3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>()); };
                if (v.contains("soften")) object.setIKSoften(it.key(), v["soften"].get<float>());
                if (v.contains("twist")) object.setIKTwist(it.key(), v["twist"].get<float>());
                if (v.contains("hinge_axis")) object.setIKHingeAxis(it.key(), vector(v["hinge_axis"]));
                if (v.contains("aim_offset")) object.setIKAimOffset(it.key(), vector(v["aim_offset"]));
            }
            done.push_back("ik " + it.key());
        }
    }
    if (!target.playing) {
        // the edit preview: evaluated at once (and advanced by advance seconds)
        std::string error;
        const float advance = args.value("advance", 0.0f);
        const int steps = std::max(1, (int)std::ceil(advance / (1.0f / 60.0f)));
        for (int i = 0; i < steps; i++) system->preview(target.entity, advance > 0.0f ? advance / (float)steps : 0.0f, &error);
    }
    if (!errors.empty()) {
        std::string text;
        for (const std::string& e : errors) text += (text.empty() ? "" : "; ") + e;
        call->fail(text + (done.empty() ? std::string() : " (applied: " + std::to_string(done.size()) + ")"), Json{{"live", liveJson(target, *instance)}});
        return;
    }
    call->ok(std::to_string(done.size()) + " change(s)" + (target.playing ? "" : " on the edit preview") + ".",
             Json{{"applied", done}, {"previewing", !target.playing}, {"live", liveJson(target, *instance)}});
}

// ------------------------------------------------------------------ edit_avatar_mask / edit_montage

void editAvatarMaskTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error, stored;
    fs::path absolute;
    if (!assetFilePath(project, args.value("path", std::string()), "an Avatar Mask (.mask)", &Util::isAvatarMaskFile, stored, absolute, error)) {
        call->fail(error);
        return;
    }
    std::error_code ec;
    std::shared_ptr<AnimatorAssetDocument> document;
    if (!fs::exists(absolute, ec)) {
        if (!args.value("create", false)) {
            call->fail("No mask at \"" + args.value("path", std::string()) + "\": pass create: true to make it.");
            return;
        }
    } else {
        document = AnimatorAssetDocument::open(project, absolute.string(), &error);
        if (!document) {
            call->fail(error);
            return;
        }
    }
    AvatarMaskDef mask = document ? document->mask() : AvatarMaskDef();
    if (args.contains("default")) {
        if (!args["default"].is_number() && !args["default"].is_boolean()) {
            call->fail("default: a weight from 0 to 1 (what no path or set body part covers).");
            return;
        }
        mask.defaultWeight = std::clamp(args["default"].is_boolean() ? (args["default"].get<bool>() ? 1.0f : 0.0f) : args["default"].get<float>(), 0.0f, 1.0f);
    }
    if (args.contains("root_hierarchy")) {
        if (!args["root_hierarchy"].is_boolean()) {
            call->fail("root_hierarchy: true (the root part also covers the GameObject and the transforms above the hips) or false.");
            return;
        }
        mask.rootHierarchy = args["root_hierarchy"].get<bool>();
    }
    if (args.contains("body_parts")) {
        if (!args["body_parts"].is_object()) {
            call->fail("body_parts: {part: weight 0..1 | true | false | null (unset: the default)}.");
            return;
        }
        for (auto it = args["body_parts"].begin(); it != args["body_parts"].end(); ++it) {
            AvatarBodyPart part;
            if (!bodyPartFromName(it.key(), part)) {
                call->fail("body part \"" + it.key() + "\": root, body, head, left_arm, right_arm, left_hand, right_hand, left_leg, right_leg, "
                           "left_fingers, right_fingers");
                return;
            }
            if (it.value().is_null()) mask.bodyParts[(int)part].reset();
            else if (it.value().is_boolean()) mask.bodyParts[(int)part] = it.value().get<bool>() ? 1.0f : 0.0f;
            else if (it.value().is_number()) mask.bodyParts[(int)part] = std::clamp(it.value().get<float>(), 0.0f, 1.0f);
            else {
                call->fail("body_parts." + it.key() + ": a weight 0..1, true, false or null.");
                return;
            }
        }
    }
    if (args.contains("from_model")) {
        // every bone path of a scene GameObject's hierarchy (weight 1 unless given)
        SceneProject* scene = McpRefs::resolveScene(project, args.contains("scene") ? args["scene"] : Json(), error);
        const Entity entity = scene && scene->scene ? McpRefs::resolveEntity(scene, args["from_model"], error) : NULL_ENTITY;
        if (entity == NULL_ENTITY) {
            call->fail("from_model: " + error);
            return;
        }
        const float weight = args.value("from_model_weight", 1.0f);
        for (const auto& [path, child] : descendantPaths(scene->scene, entity)) {
            (void)child;
            if (path.empty()) continue;
            bool listed = false;
            for (const MaskPathDef& entry : mask.paths) listed = listed || entry.path == path;
            if (!listed) mask.paths.push_back({path, weight, false});
        }
    }
    if (args.contains("paths")) {
        // {path: weight | {weight, children} | null}
        for (auto it = args["paths"].begin(); it != args["paths"].end(); ++it) {
            auto found = std::find_if(mask.paths.begin(), mask.paths.end(), [&](const MaskPathDef& e){ return e.path == it.key(); });
            if (it.value().is_null()) {
                if (found != mask.paths.end()) mask.paths.erase(found);
                continue;
            }
            MaskPathDef entry = found != mask.paths.end() ? *found : MaskPathDef{it.key(), 1.0f, false};
            if (it.value().is_number()) {
                entry.weight = it.value().get<float>();
            } else if (it.value().is_object()) {
                for (auto field = it.value().begin(); field != it.value().end(); ++field) {
                    if (field.key() == "weight" && field.value().is_number()) entry.weight = field.value().get<float>();
                    else if (field.key() == "children" && field.value().is_boolean()) entry.children = field.value().get<bool>();
                    else {
                        call->fail("paths." + it.key() + "." + field.key() + ": {weight: 0..1, children: bool}.");
                        return;
                    }
                }
            } else {
                call->fail("paths." + it.key() + ": a weight, {weight, children} or null.");
                return;
            }
            if (found != mask.paths.end()) *found = entry;
            else mask.paths.push_back(entry);
        }
    }
    if (args.contains("preview_model")) {
        if (!args["preview_model"].is_string()) {
            call->fail("preview_model: a project-relative model or prefab (\"\" clears it).");
            return;
        }
        std::string model;
        const std::string given = args["preview_model"].get<std::string>();
        if (!given.empty() && !McpValues::assetPathFromJson(project, given, model, error)) {
            call->fail("preview_model: " + error);
            return;
        }
        mask.previewModel = model;
    }
    if (!document) {
        document = AnimatorAssetDocument::create(project, absolute.string(), AnimatorAssetDocument::Kind::Mask,
                                                 "Create Avatar Mask " + absolute.filename().string(), &error);
        if (!document) {
            call->fail(error);
            return;
        }
    }
    if (!document->commit(mask, "Edit Avatar Mask " + absolute.filename().string(), std::string(), &error)) {
        call->fail(error);
        return;
    }
    call->ok(document->projectPath() + ": " + std::to_string(document->mask().paths.size()) + " path(s).", maskJson(project, *document));
}

void editMontageTool(const CallPtr& call) {
    const Json& args = call->args();
    Project* project = call->ctx().project();
    std::string error, stored;
    fs::path absolute;
    if (!assetFilePath(project, args.value("path", std::string()), "an Animation Montage (.montage)", &Util::isMontageFile, stored, absolute, error)) {
        call->fail(error);
        return;
    }
    std::error_code ec;
    std::shared_ptr<AnimatorAssetDocument> document;
    if (!fs::exists(absolute, ec)) {
        if (!args.value("create", false)) {
            call->fail("No montage at \"" + args.value("path", std::string()) + "\": pass create: true to make it.");
            return;
        }
    } else {
        document = AnimatorAssetDocument::open(project, absolute.string(), &error);
        if (!document) {
            call->fail(error);
            return;
        }
    }
    MontageDef montage = document ? document->montage() : MontageDef();
    if (args.contains("clip") && !clipRef(project, args["clip"], montage.clip, error)) {
        call->fail("clip: " + error);
        return;
    }
    if (args.contains("slot")) montage.slot = args["slot"].get<std::string>();
    if (args.contains("rate")) montage.rate = args["rate"].get<float>();
    for (const char* key : {"blend_in", "blend_out"}) {
        if (!args.contains(key)) continue;
        MontageBlendDef& blend = std::string(key) == "blend_in" ? montage.blendIn : montage.blendOut;
        blend.time = args[key].value("time", blend.time);
        if (args[key].contains("ease")) {
            EaseType ease;
            const std::string name = args[key]["ease"].get<std::string>();
            if (!easeFromName(name, ease)) {
                call->fail(std::string(key) + ".ease \"" + name + "\" is not an ease (linear, quad_out, cubic_in_out, ...)");
                return;
            }
            blend.ease = name;
        }
    }
    if (args.contains("sections")) {
        montage.sections.clear();
        for (const Json& s : args["sections"]) {
            MontageSectionDef section;
            section.name = s.value("name", std::string());
            section.start = s.value("start", 0.0f);
            // absent: the following section (normalize resolves it); null: the montage ends there
            section.next = !s.contains("next") ? std::string("\x01") : (s["next"].is_null() ? std::string() : s["next"].get<std::string>());
            if (section.name.empty() || montage.findSection(section.name) >= 0) {
                call->fail(section.name.empty() ? std::string("sections: every section needs a name")
                                                : "sections: \"" + section.name + "\" is listed twice");
                return;
            }
            montage.sections.push_back(section);
        }
    }
    if (args.contains("events")) {
        montage.events.clear();
        for (const Json& e : args["events"]) {
            MontageEventDef event;
            event.time = e.value("time", 0.0f);
            event.function = e.value("function", std::string());
            if (!isLuaIdentifier(event.function)) {
                call->fail("events: \"" + event.function + "\" is not a Lua identifier");
                return;
            }
            if (e.contains("args")) {
                for (const Json& a : e["args"]) {
                    if (a.is_boolean()) event.args.push_back(AnimationEvent::boolArg(a.get<bool>()));
                    else if (a.is_string()) event.args.push_back(AnimationEvent::stringArg(a.get<std::string>()));
                    else event.args.push_back(AnimationEvent::numberArg(a.get<double>()));
                }
            }
            montage.events.push_back(event);
        }
    }
    std::vector<ClipWarning> warnings;
    normalizeMontage(montage, &warnings);
    if (!document) {
        document = AnimatorAssetDocument::create(project, absolute.string(), AnimatorAssetDocument::Kind::Montage,
                                                 "Create Animation Montage " + absolute.filename().string(), &error);
        if (!document) {
            call->fail(error);
            return;
        }
    }
    if (!document->commit(montage, "Edit Animation Montage " + absolute.filename().string(), std::string(), &error)) {
        call->fail(error);
        return;
    }
    const MontageDef& m = document->montage();
    Json sections = Json::array();
    for (const MontageSectionDef& s : m.sections) sections.push_back({{"name", s.name}, {"start", s.start}, {"next", s.next.empty() ? Json(nullptr) : Json(s.next)}});
    call->ok(document->projectPath() + ": clip " + (m.clip.empty() ? std::string("(none)") : m.clip.path) + ", slot " + m.slot + ", " +
             std::to_string(m.sections.size()) + " section(s).",
             Json{{"path", document->projectPath()}, {"clip", refJson(m.clip)}, {"slot", m.slot}, {"rate", m.rate}, {"sections", sections},
                  {"events", m.events.size()}, {"warnings", warningsJson(warnings)}});
}

// ------------------------------------------------------------------ sample_animation_pose

void sampleAnimationPoseTool(const CallPtr& call) {
    const Json& args = call->args();
    AnimatorTarget target;
    if (!resolveAnimator(call, target)) return;
    AnimatorSystem* system = target.scene->scene->getSystem<AnimatorSystem>().get();
    const bool live = target.playing || previewing(target.scene->id, target.entity);
    const bool posing = args.contains("state") || args.contains("parameters") || args.contains("time");
    if (target.playing && posing) {
        call->fail("In play mode the live pose is sampled: leave out state, parameters and time (or use control_animator).");
        return;
    }
    std::string error;
    bool sandbox = false;
    if (!live) {
        // edit mode: evaluate on the scene, read, put the scene back
        if (!system->preview(target.entity, 0.0f, &error)) {
            call->fail("Cannot evaluate: " + error);
            return;
        }
        sandbox = true;
    }
    AnimatorInstance* instance = target.component->runtime.get();
    if (!instance || !instance->bound) {
        if (sandbox) system->restorePreview(target.entity);
        call->fail("The Animator is not bound yet (models or clips loading).");
        return;
    }
    if (posing && !target.playing) {
        if (args.contains("parameters")) {
            for (auto it = args["parameters"].begin(); it != args["parameters"].end(); ++it) {
                const Json& v = it.value();
                instance->setFloat(it.key(), v.is_boolean() ? (v.get<bool>() ? 1.0f : 0.0f) : v.get<float>());
            }
        }
        if (args.contains("state")) {
            if (!instance->play(args["state"].get<std::string>(), args.value("layer", -1), 0.0f)) {
                if (sandbox) system->restorePreview(target.entity);
                call->fail("no state \"" + args["state"].get<std::string>() + "\"");
                return;
            }
        }
        // time: seconds into the state (the state's own clip time)
        const float time = args.value("time", 0.0f);
        system->preview(target.entity, 0.0f, &error);
        if (time > 0.0f) {
            const int steps = std::max(1, (int)std::ceil(time / (1.0f / 60.0f)));
            for (int i = 0; i < steps; i++) system->preview(target.entity, time / (float)steps, &error);
        }
    }
    const Transform& root = target.scene->scene->getComponent<Transform>(target.entity);
    std::vector<std::string> wanted;
    if (args.contains("bones")) {
        for (const Json& b : args["bones"]) wanted.push_back(b.get<std::string>());
    } else if (instance->humanoid.valid) {
        for (int s = 0; s < pose::HUMAN_BONE_COUNT; s++) if (instance->humanoid.slotBones[(size_t)s] >= 0) wanted.push_back(pose::humanBoneName(pose::boneAt(s)));
    } else {
        for (const RigBone& bone : instance->rig.bones) if (!bone.path.empty()) wanted.push_back(bone.path);
    }
    Json bones = Json::object();
    std::vector<std::string> unknown;
    for (const std::string& name : wanted) {
        const int index = instance->resolveBone(name);
        if (index < 0 || (size_t)index >= instance->rigPositions().size()) {
            unknown.push_back(name);
            continue;
        }
        const Vector3 p = root.modelMatrix * instance->rigPositions()[(size_t)index];
        const Quaternion q = (root.worldRotation * instance->rigRotations()[(size_t)index]).normalized();
        bones[name] = {{"position", {p.x, p.y, p.z}}, {"rotation", {q.x, q.y, q.z, q.w}}};
    }
    Json out = {{"bones", bones}, {"state", instance->currentState(0).fullPath}, {"normalized_time", instance->currentState(0).normalizedTime}};
    if (!unknown.empty()) out["unknown_bones"] = unknown;
    if (sandbox) system->restorePreview(target.entity);
    call->ok(std::to_string(bones.size()) + " bone(s) of \"" + target.scene->scene->getEntityName(target.entity) + "\" at " +
             out["state"].get<std::string>() + (sandbox ? " (evaluated in edit mode, scene unchanged)." : "."), out);
}

} // namespace

void registerAnimatorTools(McpRegistry& registry) {
    using namespace schema;
    {
        ToolDef tool;
        tool.name = "get_animator";
        tool.title = "Get Animator";
        tool.description =
            "controller (.controller): parameters, layers, states (motion: clip or blend_tree), transitions, diagnostics "
            "(unknown parameters, unreachable states, missing targets). entity: its Animator's settings, rig (bones, skins, "
            "humanoid), clips (loaded, bound tracks) and, in play or preview, live layers (state, normalized_time, next, "
            "transition_progress), parameters, montage, root_motion, ik.";
        tool.inputSchema = object({{"controller", assetPath("")}, {"entity", entityRef()}, {"scene", sceneRef()}});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.allowedWhilePlaying = true;
        tool.handler = getAnimatorTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "edit_animator_controller";
        tool.title = "Edit Animator Controller";
        tool.description =
            "Edit a .controller: ops in order, all or nothing, one project undo step; create: true makes it (Base Layer). Ops: "
            "add_/set_/remove_/rename_parameter, add_/set_/remove_layer, add_/set_/remove_state, add_/remove_sub_state_machine, "
            "add_/set_/remove_transition, set_default_state, set_blend_tree; their fields: jasper://docs/animator. Transitions "
            "from a state, \"Any State\", \"Entry\" or \"Exit:<machine>\"; clips .anim or \"assets/models/hero.gltf#Walk\". "
            "from_player: a state per Animation Player clip.";
        tool.inputSchema = object({
            {"path", assetPath("")},
            {"create", boolean("")},
            {"from_player", entityRef("A GameObject with an Animation Player.")},
            {"ops", array(anyObject("{op, ...fields}"), "")},
            {"scene", sceneRef()}
        }, {"path"});
        tool.annotations.readOnly = false;
        tool.annotations.idempotent = false;
        tool.allowedWhilePlaying = true;
        tool.attributesUndo = true;
        tool.handler = editAnimatorControllerTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "control_animator";
        tool.title = "Control Animator";
        tool.description =
            "parameters {name: value}, triggers, play {state, duration > 0 crossfades}, montage {path, rate, "
            "section}, stop_montage, montage_section {jump | from, next}, ik {name: {target: [x,y,z] | null, weight, soften, twist, "
            "hinge_axis, aim_offset}}, "
            "layer_weights. Play mode: the game; edit mode: preview start (advance: s) / stop restores.";
        tool.inputSchema = object({
            {"entity", entityRef()}, {"scene", sceneRef()},
            {"parameters", anyObject("")}, {"triggers", array(string(""), "")}, {"reset_triggers", array(string(""), "")},
            {"play", obj({{"state", string("")}, {"layer", integer("")}, {"normalized_time", number("")}, {"duration", number("")},
                          {"fixed_duration", boolean("")}, {"offset", number("")}}, {"state"})},
            {"montage", obj({{"path", assetPath("")}, {"rate", number("")}, {"section", string("")}}, {"path"})},
            {"stop_montage", boolean("")},
            {"montage_section", obj({{"jump", string("")}, {"from", string("")}, {"next", string("")}})},
            {"ik", anyObject("")}, {"layer_weights", anyObject("")},
            {"preview", stringEnum("", {"start", "stop"})}, {"advance", number("", 0, 60)}
        }, {"entity"});
        tool.annotations.readOnly = false;
        tool.annotations.idempotent = false;
        tool.allowedWhilePlaying = true;
        tool.handler = controlAnimatorTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "edit_avatar_mask";
        tool.title = "Edit Avatar Mask";
        tool.description =
            "Edit (create: true makes) a .mask (version 2): default (what nothing covers, 0..1), body_parts {root, body, head, "
            "left_/right_ arm, hand, fingers, leg: 0..1 | bool | null unsets}, root_hierarchy, paths {\"Hips/Spine\": weight | "
            "{weight, children} | null removes} from the masked GameObject, from_model (every path under a GameObject, "
            "from_model_weight), preview_model. Rules: jasper://docs/animator. One project undo step.";
        tool.inputSchema = object({{"path", assetPath("")}, {"create", boolean("")}, {"default", number("", 0, 1)}, {"body_parts", anyObject("")},
                                   {"root_hierarchy", boolean("")}, {"paths", anyObject("")}, {"from_model", entityRef()},
                                   {"from_model_weight", number("", 0, 1)}, {"preview_model", string("")}, {"scene", sceneRef()}}, {"path"});
        tool.annotations.readOnly = false;
        tool.annotations.idempotent = false;
        tool.attributesUndo = true;
        tool.handler = editAvatarMaskTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "edit_montage";
        tool.title = "Edit Montage";
        tool.description =
            "Edit (create: true makes) a .montage: clip (.anim or model clip), slot (a layer's slot; Default = the base layer), rate, "
            "blend_in / blend_out {time, ease}, sections [{name, start s, next | null = end}] (replaces; absent next = the following), "
            "events [{time, function, args}]. One project undo step.";
        tool.inputSchema = object({
            {"path", assetPath("")}, {"create", boolean("")}, {"clip", string("")}, {"slot", string("")}, {"rate", number("")},
            {"blend_in", obj({{"time", number("")}, {"ease", string("")}})}, {"blend_out", obj({{"time", number("")}, {"ease", string("")}})},
            {"sections", array(obj({{"name", string("")}, {"start", number("")}, {"next", any("")}}, {"name"}), "")},
            {"events", array(obj({{"time", number("")}, {"function", string("")}, {"args", array(any(""), "")}}, {"function"}), "")}
        }, {"path"});
        tool.annotations.readOnly = false;
        tool.annotations.idempotent = false;
        tool.attributesUndo = true;
        tool.handler = editMontageTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "sample_animation_pose";
        tool.title = "Sample Animation Pose";
        tool.description =
            "World position and rotation [x,y,z,w] of an Animator's bones in its evaluated pose. Edit mode: state, parameters and "
            "time (seconds) choose the pose and the scene is restored after; play mode: the live pose. bones: paths or humanoid "
            "slots (left_hand); default every slot (humanoid) or every bone.";
        tool.inputSchema = object({{"entity", entityRef()}, {"scene", sceneRef()}, {"state", string("")}, {"layer", integer("")},
                                   {"parameters", anyObject("")}, {"time", number("", 0, 600)}, {"bones", array(string(""), "")}}, {"entity"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.allowedWhilePlaying = true;
        tool.handler = sampleAnimationPoseTool;
        registry.addTool(std::move(tool));
    }
}

void stopAnimatorPreviews(Project* project) {
    std::vector<PreviewState> list = previews();
    for (const PreviewState& p : list) stopPreview(project, p.sceneId, p.entity);
}

}

// ------------------------------------------------------------------ upstream's reads (get_animation, write_file)

namespace jasperengine::editor::mcp::animator_tools {

using namespace ::jasperengine::anim;
namespace fs = std::filesystem;

namespace {

constexpr size_t MAX_COVERAGE = 200;

std::string assetJson(Project* project, const std::string& stored) {
    return stored.empty() ? std::string() : McpValues::assetPathToJson(project, stored);
}

// a controller's clip as validation sees it (through its document: .anim or a model clip)
ControllerClipInfo clipInfo(Project* project, const std::string& clip) {
    ControllerClipInfo info;
    std::string error;
    std::shared_ptr<AnimationClipDocument> document = AnimationClipDocument::open(project, clip, &error);
    info.loads = document != nullptr;
    info.error = error;
    if (document) info.loops = document->data().wrap == ClipWrapMode::Loop || document->data().wrap == ClipWrapMode::PingPong;
    return info;
}

std::vector<ClipWarning> validate(Project* project, const ControllerDef& def) {
    return validateController(def, [project](const std::string& clip) { return clipInfo(project, clip); });
}

Json usersJson(Project* project, const std::vector<AnimatorAssetUser>& users) {
    Json out = Json::array();
    for (const AnimatorAssetUser& user : users) {
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

// the clips an Animation Player layer plays (its entries; -1: every layer)
std::vector<std::string> playerClips(const AnimationPlayerComponent& player, int layer) {
    std::vector<std::string> clips;
    for (const AnimationPlayerClip& entry : player.clips) {
        if ((layer < 0 || entry.layer == layer) && !entry.clip.empty()) clips.push_back(entry.clip);
    }
    return clips;
}

// the clips an Animator's controller layer plays (-1: every layer)
std::vector<std::string> animatorClips(Project* project, const AnimatorComponent& animator, int layer) {
    std::vector<std::string> clips;
    if (animator.controller.path.empty()) return clips;
    std::shared_ptr<AnimatorAssetDocument> document = AnimatorAssetDocument::open(project, animator.controller.path, nullptr);
    if (!document || !document->loadError().empty()) return clips;
    ControllerDef def = document->controller();
    for (size_t l = 0; l < def.layers.size(); l++) {
        if (layer >= 0 && (int)l != layer) continue;
        ControllerDef one;
        one.layers.push_back(def.layers[l]);
        visitControllerRefs(one, [&](AnimatorAssetRef& ref, const std::string& field) {
            if (field.find(".mask") == std::string::npos && !ref.path.empty()) clips.push_back(ref.path);
        });
    }
    return clips;
}

// the entities those clips' tracks bind below `root`, once per path
std::vector<std::pair<std::string, Entity>> boundTargets(Project* project, Scene* scene, Entity root, const std::vector<std::string>& clips) {
    std::vector<std::pair<std::string, Entity>> targets;
    std::set<std::string> seen;
    for (const std::string& clip : clips) {
        std::shared_ptr<AnimationClipDocument> document = AnimationClipDocument::open(project, clip, nullptr);
        if (!document) continue;
        for (const ClipTrackData& track : document->data().tracks) {
            if (track.channel == ClipChannel::Property || !seen.insert(track.path).second) continue;
            const Entity target = track.path.empty() ? root : resolvePath(scene, root, track.path);
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
    std::shared_ptr<AnimatorAssetDocument> document = AnimatorAssetDocument::open(project, mask, &error);
    if (document && !document->loadError().empty()) {
        error = document->loadError();
        document.reset();
    }
    if (!document) {
        entry["error"] = error + " (the layer contributes nothing)";
        return entry;
    }
    std::vector<Entity> targets;
    for (const std::pair<std::string, Entity>& target : boundTargets(project, scene, root, clips)) targets.push_back(target.second);
    const animlayersui::MaskCoverage result = animlayersui::maskCoverage(scene, root, document->mask(), targets);
    entry["covered"] = result.covered;
    entry["total"] = result.total;
    Json warnings = warningsJson(document->warnings());
    if (result.humanoidWithoutRig) {
        warnings.push_back({{"path", "body_parts"}, {"message", "the root has no humanoid rig (" + result.rigError + "): the body parts do not apply"}});
    }
    entry["warnings"] = warnings;
    return entry;
}

double r6(float value) {
    return std::round((double)value * 1e6) / 1e6;
}

} // namespace

Json controllerSummary(Project* project, const AnimatorAssetDocument& document) {
    const ControllerDef& def = document.controller();
    std::shared_ptr<const CompiledController> compiled = compileController(def);
    Json out = controllerJson(def);
    out["path"] = document.projectPath();
    if (!def.guid.empty()) out["guid"] = def.guid;
    if (document.converted()) out["read_as"] = animatorFileDialectName(document.dialect());
    out["diagnostics"] = warningsJson(compiled->diagnostics);
    out["warnings"] = warningsJson(validate(project, def));
    if (!document.warnings().empty()) out["load_warnings"] = warningsJson(document.warnings());
    out["users"] = usersJson(project, findControllerUsers(project, document.projectPath()));
    return out;
}

std::string controllerSummaryText(const AnimatorAssetDocument& document, const Json& summary) {
    const ControllerDef& def = document.controller();
    size_t states = 0;
    std::function<void(const StateMachineDef&)> count = [&](const StateMachineDef& m) {
        states += m.states.size();
        for (const StateMachineDef& sub : m.subMachines) count(sub);
    };
    for (const LayerDef& layer : def.layers) count(layer.machine);
    std::string text = document.projectPath() + ": " + std::to_string(def.parameters.size()) + " parameter(s), " +
                       std::to_string(def.layers.size()) + " layer(s), " + std::to_string(states) + " state(s), " +
                       std::to_string(summary["warnings"].size()) + " warning(s), " + std::to_string(summary["users"].size()) + " user(s)";
    if (document.converted()) text += " (read converted from " + std::string(animatorFileDialectName(document.dialect())) + ": saved as version 2 on the next edit)";
    size_t shown = 0;
    for (const Json& warning : summary["warnings"]) {
        if (shown++ >= 12) break;
        text += "\nwarning: " + warning["path"].get<std::string>() + ": " + warning["message"].get<std::string>();
    }
    return text;
}

void getController(const CallPtr& call) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    std::string error, stored;
    fs::path absolute;
    if (!args["controller"].is_string() ||
        !assetFilePath(project, args["controller"].get<std::string>(), "an Animator Controller (.controller)", &Util::isAnimatorControllerFile,
                       stored, absolute, error)) {
        call->fail(error.empty() ? std::string("controller: a project-relative .controller path, e.g. assets/anim/hero.controller.") : error);
        return;
    }
    std::error_code existsError;
    if (!fs::exists(absolute, existsError)) {
        call->fail("controller: '" + args["controller"].get<std::string>() +
                   "' does not exist (create one with edit_animator_controller {path, create: true}).");
        return;
    }
    std::shared_ptr<AnimatorAssetDocument> document = AnimatorAssetDocument::open(project, absolute.string(), &error);
    if (!document) {
        call->fail("controller: " + error + " (create one with edit_animator_controller {path, create: true}).");
        return;
    }
    const Json out = controllerSummary(project, *document);
    call->ok(controllerSummaryText(*document, out), out);
}

void getMask(const CallPtr& call) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    std::string error, stored;
    fs::path absolute;
    if (!args["mask"].is_string() ||
        !assetFilePath(project, args["mask"].get<std::string>(), "an Avatar Mask (.mask)", &Util::isAvatarMaskFile, stored, absolute, error)) {
        call->fail(error.empty() ? std::string("mask: a project-relative .mask path, e.g. assets/anim/arm.mask.") : error);
        return;
    }
    std::error_code existsError;
    if (!fs::exists(absolute, existsError)) {
        call->fail("mask: '" + args["mask"].get<std::string>() + "' does not exist (create one with edit_avatar_mask {path, create: true}).");
        return;
    }
    std::shared_ptr<AnimatorAssetDocument> document = AnimatorAssetDocument::open(project, absolute.string(), &error);
    if (!document) {
        call->fail("mask: " + error + " (create one with edit_avatar_mask {path, create: true}).");
        return;
    }
    const AvatarMaskDef& mask = document->mask();
    Json out = maskJson(project, *document);
    out["users"] = usersJson(project, findMaskUsers(project, document->projectPath()));
    char defaultText[32];
    std::snprintf(defaultText, sizeof(defaultText), "%g", r6(mask.defaultWeight));
    std::string text = document->projectPath() + ": default " + defaultText + ", " + std::to_string(out["body_parts"].size()) + " body part(s), " +
                       std::to_string(mask.paths.size()) + " path(s), " + std::to_string(out["users"].size()) + " user(s)";
    if (document->converted()) text += " (read converted from " + std::string(animatorFileDialectName(document->dialect())) + ")";

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
        HumanoidSlots slots;
        if (mask.usesHumanoid()) slots = resolveHumanoidSlots(engineScene, root);
        const std::vector<Entity> entries = bindMaskPaths(mask, engineScene, root);
        // the entities the root's player or Animator binds; without either every descendant
        std::vector<std::string> clips;
        if (const AnimationPlayerComponent* player = engineScene->findComponent<AnimationPlayerComponent>(root)) clips = playerClips(*player, -1);
        if (const AnimatorComponent* animator = engineScene->findComponent<AnimatorComponent>(root)) {
            for (const std::string& clip : animatorClips(project, *animator, -1)) clips.push_back(clip);
        }
        std::vector<std::pair<std::string, Entity>> entities = boundTargets(project, engineScene, root, clips);
        if (entities.empty()) entities = descendantPaths(engineScene, root);
        Json coverage = Json::array();
        size_t covered = 0;
        for (const std::pair<std::string, Entity>& entity : entities) {
            const MaskWeight weight = maskWeightOf(mask, engineScene, root, entity.second, entries, slots.valid ? &slots : nullptr);
            if (weight.weight > 0.0f) covered++;
            if (coverage.size() < MAX_COVERAGE) coverage.push_back({{"path", entity.first}, {"weight", r6(weight.weight)}, {"rule", weight.ruleName()}});
        }
        out["root"] = McpRefs::entityJson(scene, root);
        out["coverage"] = coverage;
        out["coverage_total"] = entities.size();
        out["covered"] = covered;
        if (mask.usesHumanoid()) {
            out["humanoid_rig"] = slots.valid;
            if (!slots.valid) out["humanoid_rig_error"] = slots.error;
        }
        text += "; on \"" + engineScene->getEntityName(root) + "\": " + std::to_string(covered) + "/" + std::to_string(entities.size()) +
                " entities covered" + (mask.usesHumanoid() && !slots.valid ? " (no humanoid rig: the body parts do not apply)" : "");
    }
    if (out.contains("warnings")) {
        for (const Json& warning : out["warnings"]) text += "\nwarning: " + warning["path"].get<std::string>() + ": " + warning["message"].get<std::string>();
    }
    call->ok(text, out);
}

void addPlayerLayers(Project* project, SceneProject* scene, Entity root, Json& item) {
    Scene* engineScene = scene->scene;
    const AnimationPlayerComponent* found = engineScene->findComponent<AnimationPlayerComponent>(root);
    if (!found) return;
    const AnimationPlayerComponent& player = *found;
    const bool playing = scene->playState != ScenePlayState::STOPPED && player.runtime;
    const std::vector<std::string> names = playerStateNames(player.clips);

    // upstream's controller players: a disabled player beside an Animator hands over to it
    if (const AnimatorComponent* animator = engineScene->findComponent<AnimatorComponent>(root)) {
        if (!player.enabled) {
            item["animator"] = {{"hands_over", true}, {"controller", assetJson(project, animator->controller.path)},
                                {"note", "this disabled player's play / crossFade / parameter calls go to the Animator (get_animator {entity})"}};
        }
    }

    // layers (the effective list) and their states; play mode: live weights and queues
    const std::vector<AnimationPlayerLayer> layers = playerLayers(player);
    Json layersJson = Json::array();
    for (size_t l = 0; l < layers.size(); l++) {
        Json states = Json::array();
        for (size_t i = 0; i < player.clips.size() && i < names.size(); i++) {
            const int layer = player.clips[i].layer >= 0 && (size_t)player.clips[i].layer < layers.size() ? player.clips[i].layer : 0;
            if ((size_t)layer == l) states.push_back(names[i]);
        }
        Json layer = {{"index", l}, {"name", layers[l].name}, {"weight", r6(l == 0 ? 1.0f : layers[l].weight)},
                      {"blend", layerBlendName(l == 0 ? AnimationLayerBlend::Override : layers[l].blend)},
                      {"mask", assetJson(project, layers[l].mask)}, {"states", states}};
        if (playing && l < player.runtime->layers.size()) {
            const LayerRuntime& runtime = player.runtime->layers[l];
            layer["live_weight"] = r6(player.runtime->layerWeight((int)l));
            if (runtime.maskPath != layers[l].mask) layer["live_mask"] = assetJson(project, runtime.maskPath);
            if (runtime.blend != layers[l].blend && l > 0) layer["live_blend"] = layerBlendName(runtime.blend);
            Json queue = Json::array();
            for (const QueuedState& queued : runtime.queue) {
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
                                                                          playerClips(player, (int)l)));
    }
    if (!coverage.empty()) item["mask_coverage"] = coverage;
}

bool checkMaskText(const std::string& text, std::vector<std::string>& warnings, std::string& error) {
    const MaskParseResult parsed = parseMask(text);
    if (!parsed.ok) {
        error = parsed.error.toString();
        return false;
    }
    if (parsed.converted()) {
        warnings.push_back(std::string("version 1 (") + animatorFileDialectName(parsed.dialect) +
                           "): read converted; the editor saves it as version 2 (write version 2: jasper://docs/animator)");
    }
    for (const ClipWarning& warning : parsed.warnings) warnings.push_back(warning.path.empty() ? warning.message : warning.path + ": " + warning.message);
    return true;
}

bool checkControllerText(Project* project, const std::string& text, std::vector<std::string>& warnings, std::string& error) {
    const ControllerParseResult parsed = parseController(text);
    if (!parsed.ok) {
        error = parsed.error.toString();
        return false;
    }
    if (parsed.converted()) {
        warnings.push_back(std::string("version 1 (") + animatorFileDialectName(parsed.dialect) +
                           "): read converted; the editor saves it as version 2 (prefer edit_animator_controller)");
    }
    for (const ClipWarning& warning : parsed.warnings) warnings.push_back(warning.path.empty() ? warning.message : warning.path + ": " + warning.message);
    for (const ClipWarning& warning : validate(project, parsed.data)) warnings.push_back(warning.path + ": " + warning.message);
    return true;
}

std::vector<ClipWarning> controllerWarnings(Project* project, const ControllerDef& def) {
    return validate(project, def);
}

Json controllerUsers(Project* project, const std::string& controllerPath) {
    return usersJson(project, findControllerUsers(project, controllerPath));
}

} // namespace jasperengine::editor::mcp::animator_tools
