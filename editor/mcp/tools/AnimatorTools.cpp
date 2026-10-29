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
#include "pool/AnimatorAssetPool.h"
#include "pose/HumanBones.h"
#include "subsystem/AnimatorSystem.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/Util.h"

#include <algorithm>
#include <cmath>
#include <set>

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
        Json state = {{"name", s.name}, {"motion", motionJson(s.motion)}, {"transitions", transitionsJson(s.transitions)}};
        if (s.speed != 1.0f) state["speed"] = s.speed;
        if (!s.speedParameter.empty()) state["speed_parameter"] = s.speedParameter;
        if (!s.timeParameter.empty()) state["time_parameter"] = s.timeParameter;
        if (s.loop != AnimatorStateLoop::Inherit) state["loop"] = stateLoopName(s.loop);
        if (!s.tag.empty()) state["tag"] = s.tag;
        if (s.cycleOffset != 0.0f) state["cycle_offset"] = s.cycleOffset;
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
    if (instance.output.hasRootMotion) {
        const Vector3 d = instance.output.rootDeltaPosition;
        out["root_motion"] = {{"delta_position", {d.x, d.y, d.z}}, {"delta_yaw", instance.output.rootDeltaYaw}};
    }
    Json ik = Json::array();
    for (size_t i = 0; i < target.component->ik.size() && i < instance.ik.size(); i++) {
        const AnimatorIKRun& run = instance.ik[i];
        Json item = {{"name", target.component->ik[i].name}, {"type", ikTypeName(target.component->ik[i].type)}};
        if (run.scriptTarget) item["target"] = {run.scriptTargetWorld.x, run.scriptTargetWorld.y, run.scriptTargetWorld.z};
        else if (run.hasTarget) item["target"] = {run.targetWorld.x, run.targetWorld.y, run.targetWorld.z};
        if (target.component->ik[i].type == AnimatorIKType::FootIK) item["grounded"] = run.hit;
        if (target.component->ik[i].type == AnimatorIKType::FootLock) item["locked"] = run.locked;
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
        std::shared_ptr<const CompiledController> compiled = compileController(document->controller());
        Json out = controllerJson(document->controller());
        out["path"] = document->projectPath();
        out["diagnostics"] = warningsJson(compiled->diagnostics);
        if (!document->warnings().empty()) out["load_warnings"] = warningsJson(document->warnings());
        size_t states = compiled->states.size();
        call->ok(document->projectPath() + ": " + std::to_string(document->controller().parameters.size()) + " parameter(s), " +
                 std::to_string(document->controller().layers.size()) + " layer(s), " + std::to_string(states) + " state(s), " +
                 std::to_string(compiled->diagnostics.size()) + " diagnostic(s).", out);
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
            error = "loop \"" + loop + "\": inherit, loop or once";
            return false;
        }
    }
    if (op.contains("additive_reference")) {
        const std::string reference = op["additive_reference"].get<std::string>();
        if (!additiveReferenceFromName(reference, s.additiveReference)) {
            error = "additive_reference \"" + reference + "\": first_frame, rest or time";
            return false;
        }
    }
    if (op.contains("additive_time")) s.additiveTime = op["additive_time"].get<float>();
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
        {"can_transition_to_self", Bool}, {"mute", Bool}, {"motion", Obj}};
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
            changes.push_back("remove_state " + location.state->name);
            location.machine->states.erase(location.machine->states.begin() + (long)location.index);
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
    }
    if (args.contains("ops")) {
        for (size_t i = 0; i < args["ops"].size(); i++) {
            if (!applyOp(project, def, args["ops"][i], changes, error)) {
                call->fail("ops[" + std::to_string(i) + "]: " + error + " (nothing was written).");
                return;
            }
        }
    }
    normalizeController(def);
    if (!document) {
        document = AnimatorAssetDocument::create(project, absolute.string(), AnimatorAssetDocument::Kind::Controller,
                                                 "Create Animator Controller " + absolute.filename().string(), &error);
        if (!document) {
            call->fail(error);
            return;
        }
        changes.insert(changes.begin(), "created");
    }
    if (!document->commit(def, "Edit Animator Controller " + absolute.filename().string(), std::string(), &error)) {
        call->fail(error);
        return;
    }
    std::shared_ptr<const CompiledController> compiled = compileController(document->controller());
    Json out = {{"path", document->projectPath()}, {"changes", changes}, {"diagnostics", warningsJson(compiled->diagnostics)},
                {"controller", controllerJson(document->controller())}};
    call->ok(document->projectPath() + ": " + std::to_string(changes.size()) + " change(s), " + std::to_string(compiled->diagnostics.size()) +
             " diagnostic(s).", out);
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
    if (args.contains("body_parts")) {
        for (auto it = args["body_parts"].begin(); it != args["body_parts"].end(); ++it) {
            AvatarBodyPart part;
            if (!bodyPartFromName(it.key(), part)) {
                call->fail("body part \"" + it.key() + "\": root, body, head, left_arm, right_arm, left_hand, right_hand, left_leg, right_leg");
                return;
            }
            mask.bodyParts[(int)part] = it.value().get<bool>();
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
            if (!listed) mask.paths.push_back({path, weight});
        }
    }
    if (args.contains("paths")) {
        for (auto it = args["paths"].begin(); it != args["paths"].end(); ++it) {
            auto found = std::find_if(mask.paths.begin(), mask.paths.end(), [&](const MaskPathDef& e){ return e.path == it.key(); });
            if (it.value().is_null()) {
                if (found != mask.paths.end()) mask.paths.erase(found);
            } else if (found != mask.paths.end()) {
                found->weight = it.value().get<float>();
            } else {
                mask.paths.push_back({it.key(), it.value().get<float>()});
            }
        }
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
    Json parts = Json::object();
    for (int i = 0; i < AVATAR_BODY_PART_COUNT; i++) parts[bodyPartName((AvatarBodyPart)i)] = document->mask().bodyParts[i];
    Json paths = Json::object();
    for (const MaskPathDef& entry : document->mask().paths) paths[entry.path] = entry.weight;
    call->ok(document->projectPath() + ": " + std::to_string(document->mask().paths.size()) + " path(s).",
             Json{{"path", document->projectPath()}, {"body_parts", parts}, {"paths", paths}});
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
            "Drive an Animator: parameters {name: value}, triggers, play {state, duration > 0 crossfades}, montage {path, rate, "
            "section}, stop_montage, montage_section {jump | from, next}, ik {name: {target: [x,y,z] | null, weight}}, "
            "layer_weights. Play mode: the game; edit mode: preview start (advance: s) / stop restores the scene.";
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
            "Edit (create: true makes) a .mask: body_parts {root, body, head, left_arm, right_arm, left_hand, right_hand, left_leg, "
            "right_leg: bool}, paths {\"Hero/Hips/Spine\": weight 0..1 | null removes} relative to the Animator's GameObject, "
            "from_model: list every path under a GameObject (from_model_weight). One project undo step.";
        tool.inputSchema = object({{"path", assetPath("")}, {"create", boolean("")}, {"body_parts", anyObject("")}, {"paths", anyObject("")},
                                   {"from_model", entityRef()}, {"from_model_weight", number("", 0, 1)}, {"scene", sceneRef()}}, {"path"});
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
