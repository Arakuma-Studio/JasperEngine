// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Upstream's (anim-layers) version 1 readers, kept as they were (their leniency and
// normalization are what upstream played), and the conversion to S21's definitions.

#include "UpstreamAnimFormats.h"

#include "animation/AnimJson.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/ClipBinding.h"

#include <algorithm>
#include <cmath>
#include <set>

using namespace jasperengine;
using namespace jasperengine::anim::upstream;

namespace{

    using KeptKeys = jasperengine::anim::upstream::KeptKeys;
    using ControllerData = jasperengine::anim::upstream::Controller;
    using ControllerLayer = jasperengine::anim::upstream::Layer;
    using ControllerState = jasperengine::anim::upstream::State;
    using ControllerTransition = jasperengine::anim::upstream::Transition;
    using ControllerCondition = jasperengine::anim::upstream::Condition;
    using ControllerParameter = jasperengine::anim::upstream::Parameter;
    using ControllerParameterType = jasperengine::anim::upstream::ParameterType;
    using BlendTree1D = jasperengine::anim::upstream::Tree;
    using BlendTreeChild = jasperengine::anim::upstream::TreeChild;
    using TransitionInterruption = jasperengine::anim::upstream::Interruption;
    using AvatarMaskData = jasperengine::anim::upstream::Mask;
    using AvatarMaskTransform = jasperengine::anim::upstream::MaskTransform;
    using AvatarMaskPart = jasperengine::anim::upstream::MaskPart;
    constexpr int AVATAR_MASK_PART_COUNT = jasperengine::anim::upstream::MASK_PART_COUNT;
    using ControllerFileParseResult = ParseResult<ControllerData>;
    using AvatarMaskParseResult = ParseResult<AvatarMaskData>;

    const char* const partNames[] = {"root", "body", "head", "left_leg", "right_leg", "left_arm", "right_arm",
                                     "left_fingers", "right_fingers"};
    // Unity's IK goals: no IK in masks (upstream spec 2)
    const char* const ikPartNames[] = {"left_foot_ik", "right_foot_ik", "left_hand_ik", "right_hand_ik"};

    float clampWeight(float weight){
        return std::isfinite(weight) ? std::min(std::max(weight, 0.0f), 1.0f) : 1.0f;
    }

    bool parameterTypeFromName(const std::string& name, ControllerParameterType& out){
        const char* const names[] = {"float", "int", "bool", "trigger"};
        for (size_t i = 0; i < 4; i++){
            if (name == names[i]){
                out = (ControllerParameterType)i;
                return true;
            }
        }
        return false;
    }

    bool conditionModeFromName(const std::string& name, anim::upstream::ConditionMode& out){
        const char* const names[] = {"if", "if_not", "greater", "less", "equals", "not_equal"};
        for (size_t i = 0; i < 6; i++){
            if (name == names[i]){
                out = (anim::upstream::ConditionMode)i;
                return true;
            }
        }
        return false;
    }

    bool interruptionFromName(const std::string& name, TransitionInterruption& out){
        const char* const names[] = {"none", "current_state", "next_state"};
        for (size_t i = 0; i < 3; i++){
            if (name == names[i]){
                out = (TransitionInterruption)i;
                return true;
            }
        }
        return false;
    }

    bool avatarMaskPartFromName(const std::string& name, AvatarMaskPart& out){
        for (int i = 0; i < AVATAR_MASK_PART_COUNT; i++){
            if (name == partNames[i]){
                out = (AvatarMaskPart)i;
                return true;
            }
        }
        return false;
    }

    void normalizeController(ControllerData& data, std::vector<ClipWarning>* warnings);
    void normalizeAvatarMask(AvatarMaskData& data, std::vector<ClipWarning>* warnings);

    using namespace jasperengine::anim::json;

    const char* const layerKeys[] = {"name", "weight", "blend", "mask", "mask_guid", "default_state", "any_state", "states", "transitions"};
    const char* const stateKeys[] = {"name", "clip", "clip_guid", "blend_tree", "speed", "speed_parameter", "wrap", "cycle_offset",
                                     "mirror", "foot_ik", "retarget", "position"};
    const char* const transitionKeys[] = {"from", "to", "conditions", "has_exit_time", "exit_time", "duration", "fixed_duration",
                                          "offset", "interruption", "can_transition_to_self"};
    const char* const treeKeys[] = {"parameter", "children", "automate_thresholds"};
    const char* const childKeys[] = {"clip", "clip_guid", "threshold", "speed", "mirror"};

    template<size_t N>
    bool known(const char* const (&keys)[N], const std::string& name){
        for (const char* key : keys){
            if (name == key) return true;
        }
        return false;
    }

    // the unknown members of an object, kept as compact JSON
    template<size_t N>
    void keepUnknown(const OJson& object, const char* const (&keys)[N], KeptKeys& out){
        for (auto it = object.begin(); it != object.end(); ++it){
            if (!known(keys, it.key())) out.push_back({it.key(), compact(*it)});
        }
    }

    bool readString(const OJson& object, const char* key, std::string& out, const std::string& path, std::vector<ClipWarning>& warnings){
        auto it = object.find(key);
        if (it == object.end()) return false;
        if (!it->is_string()){
            warn(&warnings, member(path, key), "expected a string: ignored");
            return false;
        }
        out = it->get<std::string>();
        return true;
    }

    void readFloat(const OJson& object, const char* key, float& out, const std::string& path, std::vector<ClipWarning>& warnings){
        auto it = object.find(key);
        if (it == object.end()) return;
        float value = out;
        if (!toFloat(*it, value)){
            warn(&warnings, member(path, key), "expected a number: using " + shortNumber(out));
            return;
        }
        out = value;
    }

    void readBool(const OJson& object, const char* key, bool& out, const std::string& path, std::vector<ClipWarning>& warnings){
        auto it = object.find(key);
        if (it == object.end()) return;
        if (!it->is_boolean()){
            warn(&warnings, member(path, key), std::string("expected true or false: using ") + (out ? "true" : "false"));
            return;
        }
        out = it->get<bool>();
    }

    bool readPosition(const OJson& object, const char* key, float* out, const std::string& path, std::vector<ClipWarning>& warnings){
        auto it = object.find(key);
        if (it == object.end()) return false;
        float x = 0.0f, y = 0.0f;
        if (!it->is_array() || it->size() != 2 || !toFloat((*it)[0], x) || !toFloat((*it)[1], y)){
            warn(&warnings, member(path, key), "expected [x, y]: ignored");
            return false;
        }
        out[0] = x;
        out[1] = y;
        return true;
    }

    void readCondition(const OJson& value, const std::string& path, std::vector<ControllerCondition>& out, std::vector<ClipWarning>& warnings){
        if (!value.is_object()){
            warn(&warnings, path, "expected {\"parameter\", \"mode\", \"threshold\"}: dropped");
            return;
        }
        ControllerCondition condition;
        if (!readString(value, "parameter", condition.parameter, path, warnings) || condition.parameter.empty()){
            warn(&warnings, member(path, "parameter"), "expected a parameter name: condition dropped");
            return;
        }
        std::string mode;
        if (!readString(value, "mode", mode, path, warnings) || !conditionModeFromName(mode, condition.mode)){
            warn(&warnings, member(path, "mode"), "expected greater, less, equals, not_equal, if or if_not: condition dropped");
            return;
        }
        readFloat(value, "threshold", condition.threshold, path, warnings);
        for (auto it = value.begin(); it != value.end(); ++it){
            if (it.key() != "parameter" && it.key() != "mode" && it.key() != "threshold") warn(&warnings, member(path, it.key()), "unknown key ignored");
        }
        out.push_back(condition);
    }

    void readTree(const OJson& value, const std::string& path, BlendTree1D& tree, std::vector<ClipWarning>& warnings){
        readString(value, "parameter", tree.parameter, path, warnings);
        readBool(value, "automate_thresholds", tree.automateThresholds, path, warnings);
        auto children = value.find("children");
        if (children != value.end()){
            if (!children->is_array()){
                warn(&warnings, member(path, "children"), "expected an array of {\"clip\", \"threshold\"}: ignored");
            }else{
                for (size_t i = 0; i < children->size(); i++){
                    const OJson& entry = (*children)[i];
                    const std::string childPath = indexed(path, "children", i);
                    if (!entry.is_object()){
                        warn(&warnings, childPath, "expected {\"clip\", \"threshold\"}: dropped");
                        continue;
                    }
                    BlendTreeChild child;
                    readString(entry, "clip", child.clip, childPath, warnings);
                    readString(entry, "clip_guid", child.clipGuid, childPath, warnings);
                    readFloat(entry, "threshold", child.threshold, childPath, warnings);
                    readFloat(entry, "speed", child.speed, childPath, warnings);
                    readBool(entry, "mirror", child.mirror, childPath, warnings);
                    keepUnknown(entry, childKeys, child.unknownKeys);
                    tree.children.push_back(std::move(child));
                }
            }
        }
        keepUnknown(value, treeKeys, tree.unknownKeys);
    }

    void readState(const OJson& value, const std::string& path, ControllerLayer& layer, std::vector<ClipWarning>& warnings){
        if (!value.is_object()){
            warn(&warnings, path, "expected a state object: dropped");
            return;
        }
        ControllerState state;
        if (!readString(value, "name", state.name, path, warnings) || state.name.empty()){
            warn(&warnings, member(path, "name"), "expected a state name: state dropped");
            return;
        }
        readString(value, "clip", state.clip, path, warnings);
        readString(value, "clip_guid", state.clipGuid, path, warnings);
        auto tree = value.find("blend_tree");
        if (tree != value.end()){
            if (!tree->is_object()){
                warn(&warnings, member(path, "blend_tree"), "expected {\"parameter\", \"children\"}: ignored");
            }else if (!state.clip.empty()){
                warn(&warnings, member(path, "blend_tree"), "the state has a clip: its blend tree is ignored");
            }else{
                state.hasBlendTree = true;
                readTree(*tree, member(path, "blend_tree"), state.blendTree, warnings);
            }
        }
        readFloat(value, "speed", state.speed, path, warnings);
        readString(value, "speed_parameter", state.speedParameter, path, warnings);
        std::string wrap;
        if (readString(value, "wrap", wrap, path, warnings) && !anim::wrapFromName(wrap, state.wrap)){
            warn(&warnings, member(path, "wrap"), "expected default, once, loop, ping_pong or clamp_forever: using default");
            state.wrap = ClipWrapMode::Default;
        }
        readFloat(value, "cycle_offset", state.cycleOffset, path, warnings);
        readBool(value, "mirror", state.mirror, path, warnings);
        readBool(value, "foot_ik", state.footIK, path, warnings);
        readBool(value, "retarget", state.retarget, path, warnings);
        state.hasPosition = readPosition(value, "position", state.position, path, warnings);
        keepUnknown(value, stateKeys, state.unknownKeys);
        layer.states.push_back(std::move(state));
    }

    void readTransition(const OJson& value, const std::string& path, ControllerLayer& layer, std::vector<ClipWarning>& warnings){
        if (!value.is_object()){
            warn(&warnings, path, "expected a transition object: dropped");
            return;
        }
        ControllerTransition transition;
        if (!readString(value, "from", transition.from, path, warnings) || transition.from.empty()){
            warn(&warnings, member(path, "from"), "expected a state name or \"any\": transition dropped");
            return;
        }
        if (!readString(value, "to", transition.to, path, warnings) || transition.to.empty()){
            warn(&warnings, member(path, "to"), "expected a state name: transition dropped");
            return;
        }
        auto conditions = value.find("conditions");
        if (conditions != value.end()){
            if (!conditions->is_array()){
                warn(&warnings, member(path, "conditions"), "expected an array of conditions: ignored");
            }else{
                for (size_t i = 0; i < conditions->size(); i++){
                    readCondition((*conditions)[i], indexed(path, "conditions", i), transition.conditions, warnings);
                }
            }
        }
        readBool(value, "has_exit_time", transition.hasExitTime, path, warnings);
        readFloat(value, "exit_time", transition.exitTime, path, warnings);
        readFloat(value, "duration", transition.duration, path, warnings);
        readBool(value, "fixed_duration", transition.fixedDuration, path, warnings);
        readFloat(value, "offset", transition.offset, path, warnings);
        std::string interruption;
        if (readString(value, "interruption", interruption, path, warnings) &&
            !interruptionFromName(interruption, transition.interruption)){
            warn(&warnings, member(path, "interruption"), "expected none, current_state or next_state: using none");
            transition.interruption = TransitionInterruption::None;
        }
        readBool(value, "can_transition_to_self", transition.canTransitionToSelf, path, warnings);
        keepUnknown(value, transitionKeys, transition.unknownKeys);
        layer.transitions.push_back(std::move(transition));
    }

    void readLayer(const OJson& value, const std::string& path, ControllerData& data, std::vector<ClipWarning>& warnings){
        if (!value.is_object()){
            warn(&warnings, path, "expected a layer object: dropped");
            return;
        }
        ControllerLayer layer;
        if (!readString(value, "name", layer.name, path, warnings) || layer.name.empty()){
            warn(&warnings, member(path, "name"), "expected a layer name: layer dropped");
            return;
        }
        readFloat(value, "weight", layer.weight, path, warnings);
        std::string blend;
        if (readString(value, "blend", blend, path, warnings) && !anim::layerBlendFromName(blend, layer.blend)){
            warn(&warnings, member(path, "blend"), "expected override or additive: using override");
        }
        readString(value, "mask", layer.mask, path, warnings);
        readString(value, "mask_guid", layer.maskGuid, path, warnings);
        readString(value, "default_state", layer.defaultState, path, warnings);
        auto any = value.find("any_state");
        if (any != value.end()){
            if (!any->is_object()) warn(&warnings, member(path, "any_state"), "expected {\"position\": [x, y]}: ignored");
            else layer.hasAnyStatePosition = readPosition(*any, "position", layer.anyStatePosition, member(path, "any_state"), warnings);
        }
        auto states = value.find("states");
        if (states != value.end()){
            if (!states->is_array()) warn(&warnings, member(path, "states"), "expected an array of states: ignored");
            else for (size_t i = 0; i < states->size(); i++) readState((*states)[i], indexed(path, "states", i), layer, warnings);
        }
        auto transitions = value.find("transitions");
        if (transitions != value.end()){
            if (!transitions->is_array()) warn(&warnings, member(path, "transitions"), "expected an array of transitions: ignored");
            else for (size_t i = 0; i < transitions->size(); i++) readTransition((*transitions)[i], indexed(path, "transitions", i), layer, warnings);
        }
        keepUnknown(value, layerKeys, layer.unknownKeys);
        data.layers.push_back(std::move(layer));
    }

    ControllerFileParseResult parseControllerText(const std::string& text){
        ControllerFileParseResult result;
        OJson root;
        if (!parseDocument(text, root, result.error)) return result;
        if (!checkHeader(root, anim::CONTROLLER_FORMAT, 1, "an Animator Controller", "Animator Controller", result.error)){
            return result;
        }
        std::vector<ClipWarning>& warnings = result.warnings;
        ControllerData data;
        std::string unknownNames;
        for (auto it = root.begin(); it != root.end(); ++it){
            const std::string& name = it.key();
            const OJson& value = *it;
            if (name == "format" || name == "version") continue;
            if (name == "guid"){
                if (!value.is_string() || !anim::isClipGuid(value.get<std::string>())){
                    warn(&warnings, "guid", "expected 32 lowercase hex digits: dropped (a new one is given on save)");
                }else{
                    data.guid = value.get<std::string>();
                }
            }else if (name == "parameters"){
                if (!value.is_array()){
                    warn(&warnings, "parameters", "expected an array of {\"name\", \"type\", \"default\"}: ignored");
                    continue;
                }
                for (size_t i = 0; i < value.size(); i++){
                    const OJson& entry = value[i];
                    const std::string path = indexed("", "parameters", i);
                    if (!entry.is_object()){
                        warn(&warnings, path, "expected {\"name\", \"type\", \"default\"}: dropped");
                        continue;
                    }
                    ControllerParameter parameter;
                    std::string type;
                    if (!readString(entry, "name", parameter.name, path, warnings) || parameter.name.empty()){
                        warn(&warnings, member(path, "name"), "expected a parameter name: dropped");
                        continue;
                    }
                    if (!readString(entry, "type", type, path, warnings) || !parameterTypeFromName(type, parameter.type)){
                        warn(&warnings, member(path, "type"), "expected float, int, bool or trigger: parameter dropped");
                        continue;
                    }
                    auto def = entry.find("default");
                    if (def != entry.end()){
                        float number = 0.0f;
                        if (def->is_boolean()) parameter.defaultValue = def->get<bool>() ? 1.0f : 0.0f;
                        else if (toFloat(*def, number)) parameter.defaultValue = number;
                        else warn(&warnings, member(path, "default"), "expected a number or true/false: using 0");
                    }
                    for (auto field = entry.begin(); field != entry.end(); ++field){
                        if (field.key() != "name" && field.key() != "type" && field.key() != "default"){
                            warn(&warnings, member(path, field.key()), "unknown key ignored");
                        }
                    }
                    data.parameters.push_back(parameter);
                }
            }else if (name == "layers"){
                if (!value.is_array()){
                    warn(&warnings, "layers", "expected an array of layers: ignored");
                    continue;
                }
                for (size_t i = 0; i < value.size(); i++) readLayer(value[i], indexed("", "layers", i), data, warnings);
            }else{
                data.unknownKeys.push_back({name, compact(value)});
                unknownNames += (unknownNames.empty() ? "" : ", ") + name;
            }
        }
        if (!unknownNames.empty()){
            warn(&warnings, "", "unknown keys kept as they are (written back on save): " + unknownNames);
        }
        normalizeController(data, &warnings);
        result.data = std::move(data);
        return result;
    }

    float finite(float value, float fallback){
        return std::isfinite(value) ? value : fallback;
    }

    AvatarMaskParseResult parseMaskText(const std::string& text){
        AvatarMaskParseResult result;
        OJson root;
        if (!parseDocument(text, root, result.error)) return result;
        if (!checkHeader(root, anim::MASK_FORMAT, 1, "an Avatar Mask", "Avatar Mask", result.error)) return result;

        std::vector<ClipWarning>& warnings = result.warnings;
        AvatarMaskData data;
        std::string unknownNames;
        for (auto it = root.begin(); it != root.end(); ++it){
            const std::string& name = it.key();
            const OJson& value = *it;
            if (name == "format" || name == "version") continue;
            if (name == "guid"){
                if (!value.is_string() || !anim::isClipGuid(value.get<std::string>())){
                    warn(&warnings, "guid", "expected 32 lowercase hex digits: dropped (a new one is given on save)");
                }else{
                    data.guid = value.get<std::string>();
                }
            }else if (name == "default"){
                float weight = 1.0f;
                if (!toWeight(value, weight)){
                    warn(&warnings, "default", "expected a weight from 0 to 1: using 1");
                    weight = 1.0f;
                }
                data.defaultWeight = weight;
            }else if (name == "humanoid"){
                if (!value.is_object()){
                    warn(&warnings, "humanoid", "expected {\"<body part>\": weight, ...}: ignored");
                    continue;
                }
                data.hasHumanoid = true;
                for (auto part = value.begin(); part != value.end(); ++part){
                    const std::string path = member("humanoid", part.key());
                    AvatarMaskPart which;
                    if (!avatarMaskPartFromName(part.key(), which)){
                        bool ik = false;
                        for (const char* ikName : ikPartNames){
                            if (part.key() == ikName) ik = true;
                        }
                        warn(&warnings, path, ik ? "IK goals are not masked (there is no IK pass): ignored"
                                                 : "unknown body part (root, body, head, left_leg, right_leg, left_arm, right_arm, "
                                                   "left_fingers, right_fingers): ignored");
                        continue;
                    }
                    float weight = 0.0f;
                    if (!toWeight(*part, weight)){
                        warn(&warnings, path, "expected a weight from 0 to 1 (or true/false): ignored");
                        continue;
                    }
                    data.parts[(size_t)which] = weight;
                }
            }else if (name == "transforms"){
                if (!value.is_array()){
                    warn(&warnings, "transforms", "expected an array of {\"path\", \"weight\", \"children\"}: ignored");
                    continue;
                }
                for (size_t i = 0; i < value.size(); i++){
                    const OJson& entry = value[i];
                    const std::string path = indexed("", "transforms", i);
                    if (!entry.is_object()){
                        warn(&warnings, path, "expected {\"path\", \"weight\", \"children\"}: dropped");
                        continue;
                    }
                    AvatarMaskTransform transform;
                    auto pathIt = entry.find("path");
                    std::vector<anim::PathSegment> segments;
                    if (pathIt == entry.end() || !pathIt->is_string()){
                        warn(&warnings, member(path, "path"), "expected a path relative to the player root: entry dropped");
                        continue;
                    }
                    transform.path = pathIt->get<std::string>();
                    if (!transform.path.empty() && !anim::parsePath(transform.path, segments)){
                        warn(&warnings, member(path, "path"), "\"" + transform.path + "\" is not a valid path: entry dropped");
                        continue;
                    }
                    for (auto field = entry.begin(); field != entry.end(); ++field){
                        if (field.key() == "path") continue;
                        if (field.key() == "weight"){
                            if (!toWeight(*field, transform.weight)){
                                warn(&warnings, member(path, "weight"), "expected a weight from 0 to 1: using 1");
                                transform.weight = 1.0f;
                            }
                        }else if (field.key() == "children"){
                            if (!field->is_boolean()){
                                warn(&warnings, member(path, "children"), "expected true or false: using true");
                            }else{
                                transform.children = field->get<bool>();
                            }
                        }else{
                            warn(&warnings, member(path, field.key()), "unknown key ignored");
                        }
                    }
                    data.transforms.push_back(std::move(transform));
                }
            }else if (name == "preview"){
                if (!value.is_object()){
                    warn(&warnings, "preview", "expected {\"model\": \"<model or prefab path>\"}: ignored");
                    continue;
                }
                for (auto entry = value.begin(); entry != value.end(); ++entry){
                    if (entry.key() == "model" && entry->is_string()){
                        data.previewModel = entry->get<std::string>();
                    }else if (entry.key() == "model"){
                        warn(&warnings, "preview.model", "expected a model or prefab path: ignored");
                    }else{
                        warn(&warnings, "preview." + entry.key(), "unknown key ignored");
                    }
                }
            }else{
                data.unknownKeys.push_back({name, compact(value)});
                unknownNames += (unknownNames.empty() ? "" : ", ") + name;
            }
        }
        if (!unknownNames.empty()){
            warn(&warnings, "", "unknown keys kept as they are (written back on save): " + unknownNames);
        }
        normalizeAvatarMask(data, &warnings);
        result.data = std::move(data);
        return result;
    }

    void normalizeController(ControllerData& data, std::vector<ClipWarning>* warnings){
        if (!data.guid.empty() && !anim::isClipGuid(data.guid)){
            warn(warnings, "guid", "not 32 lowercase hex digits: dropped");
            data.guid.clear();
        }
        // parameters: unique names, defaults of their type
        std::vector<ControllerParameter> parameters;
        for (size_t i = 0; i < data.parameters.size(); i++){
            ControllerParameter parameter = data.parameters[i];
            const std::string path = indexed("", "parameters", i);
            if (parameter.name.empty()){
                warn(warnings, path, "a parameter without a name: dropped");
                continue;
            }
            // Lua-safe (spec 11.1): scripts name them as strings and the editor as identifiers
            if (!anim::isLuaIdentifier(parameter.name)){
                warn(warnings, path, "\"" + parameter.name + "\" is not a Lua-safe name (letters, digits and _, not starting with a digit, "
                                     "not a Lua keyword): dropped");
                continue;
            }
            bool taken = false;
            for (const ControllerParameter& kept : parameters) taken |= kept.name == parameter.name;
            if (taken){
                warn(warnings, path, "\"" + parameter.name + "\" is already a parameter: dropped");
                continue;
            }
            float value = finite(parameter.defaultValue, 0.0f);
            switch (parameter.type){
                case ControllerParameterType::Float: break;
                case ControllerParameterType::Int: value = std::round(value); break;
                case ControllerParameterType::Bool: value = value != 0.0f ? 1.0f : 0.0f; break;
                case ControllerParameterType::Trigger: value = 0.0f; break;
            }
            parameter.defaultValue = value;
            parameters.push_back(parameter);
        }
        data.parameters = std::move(parameters);
    
        if (data.layers.empty()){
            ControllerLayer base;
            base.name = anim::BASE_LAYER_NAME;
            data.layers.push_back(base);
        }
        std::vector<ControllerLayer> layers;
        for (size_t l = 0; l < data.layers.size(); l++){
            ControllerLayer layer = data.layers[l];
            const std::string path = indexed("", "layers", l);
            bool taken = false;
            for (const ControllerLayer& kept : layers) taken |= kept.name == layer.name;
            if (layer.name.empty() || taken){
                warn(warnings, path, layer.name.empty() ? "a layer without a name: dropped" : "\"" + layer.name + "\" is already a layer: dropped");
                continue;
            }
            const float weight = finite(layer.weight, 1.0f);
            layer.weight = std::min(std::max(weight, 0.0f), 1.0f);
            if (layer.weight != weight) warn(warnings, member(path, "weight"), "outside 0..1: using " + shortNumber(layer.weight));
    
            // states: unique names, not the reserved "any"
            std::vector<ControllerState> states;
            for (size_t s = 0; s < layer.states.size(); s++){
                ControllerState state = layer.states[s];
                const std::string statePath = indexed(path, "states", s);
                bool duplicate = false;
                for (const ControllerState& kept : states) duplicate |= kept.name == state.name;
                if (state.name.empty() || state.name == anim::upstream::ANY_STATE || duplicate){
                    warn(warnings, statePath, state.name.empty() ? "a state without a name: dropped" :
                                              state.name == anim::upstream::ANY_STATE ? "\"any\" names the Any State: state dropped" :
                                              "\"" + state.name + "\" is already a state of the layer: dropped");
                    continue;
                }
                state.speed = finite(state.speed, 1.0f);
                const float offset = finite(state.cycleOffset, 0.0f);
                state.cycleOffset = std::min(std::max(offset, 0.0f), 1.0f);
                if (state.cycleOffset != offset) warn(warnings, member(statePath, "cycle_offset"), "outside 0..1: using " + shortNumber(state.cycleOffset));
                if (!state.clip.empty() && state.hasBlendTree){
                    state.hasBlendTree = false;
                    state.blendTree = BlendTree1D();
                }
                if (state.clip.empty()) state.clipGuid.clear();
                if (state.hasBlendTree){
                    BlendTree1D& tree = state.blendTree;
                    for (BlendTreeChild& child : tree.children){
                        child.threshold = finite(child.threshold, 0.0f);
                        child.speed = finite(child.speed, 1.0f);
                        if (child.clip.empty()) child.clipGuid.clear();
                    }
                    if (tree.automateThresholds){
                        const size_t n = tree.children.size();
                        for (size_t c = 0; c < n; c++) tree.children[c].threshold = n > 1 ? (float)c / (float)(n - 1) : 0.0f;
                    }else{
                        std::stable_sort(tree.children.begin(), tree.children.end(), [](const BlendTreeChild& a, const BlendTreeChild& b){
                            return a.threshold < b.threshold;
                        });
                        for (size_t c = 1; c < tree.children.size(); c++){
                            if (tree.children[c].threshold == tree.children[c - 1].threshold){
                                warn(warnings, indexed(member(statePath, "blend_tree"), "children", c),
                                     "the same threshold as the child before it (" + shortNumber(tree.children[c].threshold) + "): it never plays alone");
                            }
                        }
                    }
                }
                states.push_back(std::move(state));
            }
            layer.states = std::move(states);
            if (!layer.defaultState.empty() && layer.findState(layer.defaultState) < 0){
                warn(warnings, member(path, "default_state"), "\"" + layer.defaultState + "\" is not a state of the layer: the first state is the default");
                layer.defaultState.clear();
            }
    
            // transitions: existing states
            std::vector<ControllerTransition> transitions;
            for (size_t t = 0; t < layer.transitions.size(); t++){
                ControllerTransition transition = layer.transitions[t];
                const std::string transitionPath = indexed(path, "transitions", t);
                if (transition.from != anim::upstream::ANY_STATE && layer.findState(transition.from) < 0){
                    warn(warnings, member(transitionPath, "from"), "\"" + transition.from + "\" is not a state of the layer (or \"any\"): transition dropped");
                    continue;
                }
                if (layer.findState(transition.to) < 0){
                    warn(warnings, member(transitionPath, "to"), "\"" + transition.to + "\" is not a state of the layer: transition dropped");
                    continue;
                }
                transition.exitTime = std::max(0.0f, finite(transition.exitTime, 0.75f));
                transition.duration = std::max(0.0f, finite(transition.duration, 0.25f));
                transition.offset = std::min(std::max(finite(transition.offset, 0.0f), 0.0f), 1.0f);
                for (ControllerCondition& condition : transition.conditions) condition.threshold = finite(condition.threshold, 0.0f);
                if (transition.from != anim::upstream::ANY_STATE) transition.canTransitionToSelf = true;
                transitions.push_back(std::move(transition));
            }
            layer.transitions = std::move(transitions);
            layers.push_back(std::move(layer));
        }
        if (layers.empty()){
            ControllerLayer base;
            base.name = anim::BASE_LAYER_NAME;
            layers.push_back(base);
        }
        data.layers = std::move(layers);
    }

    void normalizeAvatarMask(AvatarMaskData& data, std::vector<ClipWarning>* warnings){
        auto clamped = [&](float value, const std::string& path){
            const float result = clampWeight(value);
            if (result != value) warn(warnings, path, shortNumber(value) + " is outside 0..1, using " + shortNumber(result));
            return result;
        };
        data.defaultWeight = clamped(data.defaultWeight, "default");
        for (int i = 0; i < AVATAR_MASK_PART_COUNT; i++){
            if (data.parts[(size_t)i]) data.parts[(size_t)i] = clamped(*data.parts[(size_t)i], member("humanoid", partNames[i]));
            if (data.parts[(size_t)i]) data.hasHumanoid = true;
        }
        if (!data.guid.empty() && !anim::isClipGuid(data.guid)){
            warn(warnings, "guid", "not 32 lowercase hex digits: dropped");
            data.guid.clear();
        }
        // sorted by path (byte order), one entry per path: the later one wins
        std::vector<AvatarMaskTransform> transforms;
        for (size_t i = 0; i < data.transforms.size(); i++){
            AvatarMaskTransform entry = data.transforms[i];
            entry.weight = clamped(entry.weight, indexed("", "transforms", i) + ".weight");
            auto existing = std::find_if(transforms.begin(), transforms.end(), [&](const AvatarMaskTransform& t){ return t.path == entry.path; });
            if (existing != transforms.end()){
                warn(warnings, indexed("", "transforms", i), "\"" + entry.path + "\" is listed twice: the later entry is used");
                *existing = entry;
            }else{
                transforms.push_back(entry);
            }
        }
        std::stable_sort(transforms.begin(), transforms.end(), [](const AvatarMaskTransform& a, const AvatarMaskTransform& b){
            return a.path < b.path;
        });
        data.transforms = std::move(transforms);
    }

}

int anim::upstream::Layer::findState(const std::string& name) const{
    for (size_t i = 0; i < states.size(); i++){
        if (states[i].name == name) return (int)i;
    }
    return -1;
}

const char* anim::upstream::maskPartName(MaskPart part){
    const size_t index = (size_t)part;
    return index < (size_t)MASK_PART_COUNT ? partNames[index] : "";
}

anim::upstream::ParseResult<anim::upstream::Controller> anim::upstream::parseController(const std::string& text){
    try{
        return parseControllerText(text);
    }catch (const std::exception& e){
        ParseResult<Controller> result;
        result.error.message = std::string("cannot read the Animator Controller: ") + e.what();
        return result;
    }
}

anim::upstream::ParseResult<anim::upstream::Mask> anim::upstream::parseMask(const std::string& text){
    try{
        return parseMaskText(text);
    }catch (const std::exception& e){
        ParseResult<Mask> result;
        result.error.message = std::string("cannot read the Avatar Mask: ") + e.what();
        return result;
    }
}

// ---- conversion ----

namespace{

    AnimatorParameterType toParameterType(ControllerParameterType type){
        switch (type){
            case ControllerParameterType::Float: return AnimatorParameterType::Float;
            case ControllerParameterType::Int: return AnimatorParameterType::Int;
            case ControllerParameterType::Bool: return AnimatorParameterType::Bool;
            case ControllerParameterType::Trigger: return AnimatorParameterType::Trigger;
        }
        return AnimatorParameterType::Float;
    }

    AnimatorConditionMode toConditionMode(anim::upstream::ConditionMode mode){
        switch (mode){
            case anim::upstream::ConditionMode::If: return AnimatorConditionMode::If;
            case anim::upstream::ConditionMode::IfNot: return AnimatorConditionMode::IfNot;
            case anim::upstream::ConditionMode::Greater: return AnimatorConditionMode::Greater;
            case anim::upstream::ConditionMode::Less: return AnimatorConditionMode::Less;
            case anim::upstream::ConditionMode::Equals: return AnimatorConditionMode::Equals;
            case anim::upstream::ConditionMode::NotEqual: return AnimatorConditionMode::NotEqual;
        }
        return AnimatorConditionMode::If;
    }

    AnimatorStateLoop toLoop(ClipWrapMode wrap){
        switch (wrap){
            case ClipWrapMode::Default: return AnimatorStateLoop::Inherit;
            case ClipWrapMode::Once: return AnimatorStateLoop::Once;
            // upstream's state wrap replaced the clip's: a ping-pong clip loops forward too
            case ClipWrapMode::Loop: return AnimatorStateLoop::Repeat;
            case ClipWrapMode::PingPong: return AnimatorStateLoop::PingPong;
            case ClipWrapMode::ClampForever: return AnimatorStateLoop::ClampForever;
        }
        return AnimatorStateLoop::Inherit;
    }

    anim::AnimatorAssetRef ref(const std::string& path, const std::string& guid){
        anim::AnimatorAssetRef out;
        out.path = path;
        out.guid = path.empty() ? std::string() : guid;
        return out;
    }

    anim::TransitionDef toTransition(const ControllerTransition& t, bool anyState){
        anim::TransitionDef out;
        out.to = t.to;
        for (const ControllerCondition& c : t.conditions){
            anim::ConditionDef condition;
            condition.parameter = c.parameter;
            condition.mode = toConditionMode(c.mode);
            condition.threshold = c.threshold;
            out.conditions.push_back(condition);
        }
        out.hasExitTime = t.hasExitTime;
        out.exitTime = t.exitTime;
        out.fixedDuration = t.fixedDuration;
        out.duration = t.duration;
        out.offset = t.offset;
        switch (t.interruption){
            case TransitionInterruption::None: out.interruptionSource = TransitionInterruptionSource::None; break;
            case TransitionInterruption::CurrentState: out.interruptionSource = TransitionInterruptionSource::Current; break;
            case TransitionInterruption::NextState: out.interruptionSource = TransitionInterruptionSource::Next; break;
        }
        // upstream's current_state source checked the transitions listed before the active one
        out.orderedInterruption = true;
        out.canTransitionToSelf = anyState && t.canTransitionToSelf;
        out.unknownKeys = t.unknownKeys;
        return out;
    }

}

anim::ControllerDef anim::upstream::toControllerDef(const Controller& data, std::vector<ClipWarning>* warnings){
    ControllerDef def;
    def.guid = data.guid;
    for (const Parameter& p : data.parameters){
        ParameterDef parameter;
        parameter.name = p.name;
        parameter.type = toParameterType(p.type);
        parameter.defaultValue = p.defaultValue;
        def.parameters.push_back(parameter);
    }
    for (size_t l = 0; l < data.layers.size(); l++){
        const Layer& layer = data.layers[l];
        const std::string layerPath = "layers[" + std::to_string(l) + "]";
        LayerDef out;
        out.name = layer.name;
        out.weight = layer.weight;
        out.blending = layer.blend == AnimationLayerBlend::Additive ? AnimatorLayerBlending::Additive : AnimatorLayerBlending::Override;
        out.mask = ref(layer.mask, layer.maskGuid);
        out.unknownKeys = layer.unknownKeys;
        StateMachineDef& machine = out.machine;
        machine.defaultState = layer.defaultState;
        if (layer.hasAnyStatePosition) machine.anyStatePosition = Vector2(layer.anyStatePosition[0], layer.anyStatePosition[1]);
        for (size_t s = 0; s < layer.states.size(); s++){
            const State& state = layer.states[s];
            StateDef converted;
            converted.name = state.name;
            // a state without a graph position: a grid right of Entry and Any State
            converted.position = state.hasPosition ? Vector2(state.position[0], state.position[1])
                                                   : Vector2(250.0f + (float)(s % 4) * 220.0f, 60.0f + (float)(s / 4) * 100.0f);
            if (state.hasBlendTree){
                BlendTreeDef tree;
                tree.type = BlendTreeType::Simple1D;
                tree.parameter = state.blendTree.parameter;
                for (const TreeChild& child : state.blendTree.children){
                    BlendChildDef c;
                    c.motion.clip = ref(child.clip, child.clipGuid);
                    c.threshold = child.threshold;
                    c.timeScale = child.speed;
                    c.unknownKeys = child.unknownKeys;
                    c.mirror = child.mirror;
                    tree.children.push_back(std::move(c));
                }
                tree.unknownKeys = state.blendTree.unknownKeys;
                if (state.blendTree.automateThresholds) tree.unknownKeys.push_back({"automate_thresholds", "true"});
                converted.motion.tree.push_back(std::move(tree));
            }else if (!state.clip.empty()){
                converted.motion.clip = ref(state.clip, state.clipGuid);
            }
            converted.speed = state.speed;
            converted.speedParameter = state.speedParameter;
            converted.cycleOffset = state.cycleOffset;
            converted.loop = toLoop(state.wrap);
            // upstream's additive layers measured from the clip's own additive_reference
            converted.additiveReference = AdditiveReference::Clip;
            converted.unknownKeys = state.unknownKeys;
            // humanoid playback (anim-retarget): S21's state flags, played on the one retarget path
            converted.mirror = state.mirror;
            converted.footIK = state.footIK;
            converted.retarget = state.retarget;
            machine.states.push_back(std::move(converted));
        }
        for (size_t t = 0; t < layer.transitions.size(); t++){
            const Transition& transition = layer.transitions[t];
            const bool any = transition.from == ANY_STATE;
            if (transition.conditions.empty() && !transition.hasExitTime && warnings){
                warnings->push_back({layerPath + ".transitions[" + std::to_string(t) + "]",
                                     "neither conditions nor an exit time: upstream fired it at once, this Animator (as Unity) never takes it "
                                     "(give it a condition or an exit time)"});
            }
            if (any){
                machine.anyState.push_back(toTransition(transition, true));
                continue;
            }
            const int from = layer.findState(transition.from);
            if (from >= 0) machine.states[(size_t)from].transitions.push_back(toTransition(transition, false));
        }
        def.layers.push_back(std::move(out));
    }
    def.unknownKeys = data.unknownKeys;
    return def;
}

anim::AvatarMaskDef anim::upstream::toMaskDef(const Mask& data){
    AvatarMaskDef def;
    def.guid = data.guid;
    def.defaultWeight = data.defaultWeight;
    auto part = [&](MaskPart from, std::initializer_list<AvatarBodyPart> to){
        const std::optional<float>& value = data.parts[(size_t)from];
        if (!data.hasHumanoid || !value) return;
        for (AvatarBodyPart p : to) def.bodyParts[(int)p] = *value;
    };
    part(MaskPart::Root, {AvatarBodyPart::Root});
    part(MaskPart::Body, {AvatarBodyPart::Body});
    part(MaskPart::Head, {AvatarBodyPart::Head});
    part(MaskPart::LeftLeg, {AvatarBodyPart::LeftLeg});
    part(MaskPart::RightLeg, {AvatarBodyPart::RightLeg});
    // upstream's arm held the hand bone; its fingers what is below the hand
    part(MaskPart::LeftArm, {AvatarBodyPart::LeftArm, AvatarBodyPart::LeftHand});
    part(MaskPart::RightArm, {AvatarBodyPart::RightArm, AvatarBodyPart::RightHand});
    part(MaskPart::LeftFingers, {AvatarBodyPart::LeftFingers});
    part(MaskPart::RightFingers, {AvatarBodyPart::RightFingers});
    def.rootHierarchy = true;
    for (const MaskTransform& transform : data.transforms){
        MaskPathDef entry;
        entry.path = transform.path;
        entry.weight = transform.weight;
        entry.children = transform.children;
        def.paths.push_back(entry);
    }
    def.previewModel = data.previewModel;
    def.unknownKeys = data.unknownKeys;
    return def;
}
