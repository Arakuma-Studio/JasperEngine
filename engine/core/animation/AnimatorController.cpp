// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimatorController.h"

#include "animation/AnimJson.h"
#include "animation/AnimationPlayerRuntime.h"

#include <algorithm>
#include <cmath>
#include <set>

using namespace jasperengine;

namespace{

    using namespace jasperengine::anim::json;
    using anim::KeptKeys;

    const char* const typeNames[] = {"float", "int", "bool", "trigger"};
    const char* const modeNames[] = {"if", "if_not", "greater", "less", "equals", "not_equal"};
    const char* const interruptionNames[] = {"none", "current_state", "next_state"};

    const char* const topKeys[] = {"format", "version", "guid", "parameters", "layers"};
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

    // the kept members as `, "key": value` (compact, the last of a repeated name)
    template<size_t N>
    std::string keptText(const KeptKeys& kept, const char* const (&keys)[N]){
        std::string text;
        for (size_t i = 0; i < kept.size(); i++){
            if (known(keys, kept[i].first)) continue;
            bool repeated = false;
            for (size_t j = i + 1; j < kept.size(); j++){
                if (kept[j].first == kept[i].first) repeated = true;
            }
            if (repeated) continue;
            text += ", " + quote(kept[i].first) + ": " + (kept[i].second.empty() ? std::string("null") : kept[i].second);
        }
        return text;
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

    std::string positionText(const float* position){
        return "[" + number(position[0]) + ", " + number(position[1]) + "]";
    }

    void readCondition(const OJson& value, const std::string& path, std::vector<anim::ControllerCondition>& out, std::vector<ClipWarning>& warnings){
        if (!value.is_object()){
            warn(&warnings, path, "expected {\"parameter\", \"mode\", \"threshold\"}: dropped");
            return;
        }
        anim::ControllerCondition condition;
        if (!readString(value, "parameter", condition.parameter, path, warnings) || condition.parameter.empty()){
            warn(&warnings, member(path, "parameter"), "expected a parameter name: condition dropped");
            return;
        }
        std::string mode;
        if (!readString(value, "mode", mode, path, warnings) || !anim::conditionModeFromName(mode, condition.mode)){
            warn(&warnings, member(path, "mode"), "expected greater, less, equals, not_equal, if or if_not: condition dropped");
            return;
        }
        readFloat(value, "threshold", condition.threshold, path, warnings);
        for (auto it = value.begin(); it != value.end(); ++it){
            if (it.key() != "parameter" && it.key() != "mode" && it.key() != "threshold") warn(&warnings, member(path, it.key()), "unknown key ignored");
        }
        out.push_back(condition);
    }

    void readTree(const OJson& value, const std::string& path, anim::BlendTree1D& tree, std::vector<ClipWarning>& warnings){
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
                    anim::BlendTreeChild child;
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

    void readState(const OJson& value, const std::string& path, anim::ControllerLayer& layer, std::vector<ClipWarning>& warnings){
        if (!value.is_object()){
            warn(&warnings, path, "expected a state object: dropped");
            return;
        }
        anim::ControllerState state;
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

    void readTransition(const OJson& value, const std::string& path, anim::ControllerLayer& layer, std::vector<ClipWarning>& warnings){
        if (!value.is_object()){
            warn(&warnings, path, "expected a transition object: dropped");
            return;
        }
        anim::ControllerTransition transition;
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
            !anim::interruptionFromName(interruption, transition.interruption)){
            warn(&warnings, member(path, "interruption"), "expected none, current_state or next_state: using none");
            transition.interruption = anim::TransitionInterruption::None;
        }
        readBool(value, "can_transition_to_self", transition.canTransitionToSelf, path, warnings);
        keepUnknown(value, transitionKeys, transition.unknownKeys);
        layer.transitions.push_back(std::move(transition));
    }

    void readLayer(const OJson& value, const std::string& path, anim::ControllerData& data, std::vector<ClipWarning>& warnings){
        if (!value.is_object()){
            warn(&warnings, path, "expected a layer object: dropped");
            return;
        }
        anim::ControllerLayer layer;
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

    anim::ControllerParseResult parseText(const std::string& text){
        anim::ControllerParseResult result;
        if (anim::isUnityYamlController(text)){
            result.error.message = anim::unityYamlControllerRefusal();
            return result;
        }
        OJson root;
        if (!parseDocument(text, root, result.error)) return result;
        if (!checkHeader(root, anim::CONTROLLER_FORMAT, anim::CONTROLLER_VERSION, "an Animator Controller", "Animator Controller", result.error)){
            return result;
        }
        std::vector<ClipWarning>& warnings = result.warnings;
        anim::ControllerData data;
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
                    anim::ControllerParameter parameter;
                    std::string type;
                    if (!readString(entry, "name", parameter.name, path, warnings) || parameter.name.empty()){
                        warn(&warnings, member(path, "name"), "expected a parameter name: dropped");
                        continue;
                    }
                    if (!readString(entry, "type", type, path, warnings) || !anim::parameterTypeFromName(type, parameter.type)){
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
        anim::normalizeController(data, &warnings);
        result.data = std::move(data);
        return result;
    }

    float finite(float value, float fallback){
        return std::isfinite(value) ? value : fallback;
    }

}

const char* anim::parameterTypeName(ControllerParameterType type){
    const size_t index = (size_t)type;
    return index < 4 ? typeNames[index] : "";
}

bool anim::parameterTypeFromName(const std::string& name, ControllerParameterType& out){
    for (size_t i = 0; i < 4; i++){
        if (name == typeNames[i]){
            out = (ControllerParameterType)i;
            return true;
        }
    }
    return false;
}

const char* anim::conditionModeName(ConditionMode mode){
    const size_t index = (size_t)mode;
    return index < 6 ? modeNames[index] : "";
}

bool anim::conditionModeFromName(const std::string& name, ConditionMode& out){
    for (size_t i = 0; i < 6; i++){
        if (name == modeNames[i]){
            out = (ConditionMode)i;
            return true;
        }
    }
    return false;
}

bool anim::conditionModeFits(ConditionMode mode, ControllerParameterType type){
    switch (mode){
        case ConditionMode::Greater:
        case ConditionMode::Less:
            return type == ControllerParameterType::Float || type == ControllerParameterType::Int;
        case ConditionMode::Equals:
        case ConditionMode::NotEqual:
            return type == ControllerParameterType::Int;
        case ConditionMode::If:
            return type == ControllerParameterType::Bool || type == ControllerParameterType::Trigger;
        case ConditionMode::IfNot:
            return type == ControllerParameterType::Bool;
    }
    return false;
}

const char* anim::interruptionName(TransitionInterruption interruption){
    const size_t index = (size_t)interruption;
    return index < 3 ? interruptionNames[index] : "";
}

bool anim::interruptionFromName(const std::string& name, TransitionInterruption& out){
    for (size_t i = 0; i < 3; i++){
        if (name == interruptionNames[i]){
            out = (TransitionInterruption)i;
            return true;
        }
    }
    return false;
}

int anim::ControllerLayer::findState(const std::string& name) const{
    for (size_t i = 0; i < states.size(); i++){
        if (states[i].name == name) return (int)i;
    }
    return -1;
}

int anim::ControllerLayer::defaultStateIndex() const{
    if (states.empty()) return -1;
    const int index = findState(defaultState);
    return index >= 0 ? index : 0;
}

int anim::ControllerData::findParameter(const std::string& name) const{
    for (size_t i = 0; i < parameters.size(); i++){
        if (parameters[i].name == name) return (int)i;
    }
    return -1;
}

int anim::ControllerData::findLayer(const std::string& name) const{
    for (size_t i = 0; i < layers.size(); i++){
        if (layers[i].name == name) return (int)i;
    }
    return -1;
}

void anim::normalizeController(ControllerData& data, std::vector<ClipWarning>* warnings){
    if (!data.guid.empty() && !isClipGuid(data.guid)){
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
        base.name = BASE_LAYER_NAME;
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
            if (state.name.empty() || state.name == ANY_STATE || duplicate){
                warn(warnings, statePath, state.name.empty() ? "a state without a name: dropped" :
                                          state.name == ANY_STATE ? "\"any\" names the Any State: state dropped" :
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
            if (transition.from != ANY_STATE && layer.findState(transition.from) < 0){
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
            if (transition.from != ANY_STATE) transition.canTransitionToSelf = true;
            transitions.push_back(std::move(transition));
        }
        layer.transitions = std::move(transitions);
        layers.push_back(std::move(layer));
    }
    if (layers.empty()){
        ControllerLayer base;
        base.name = BASE_LAYER_NAME;
        layers.push_back(base);
    }
    data.layers = std::move(layers);
}

anim::ControllerParseResult anim::parseController(const std::string& text){
    try{
        return parseText(text);
    }catch (const std::exception& e){
        ControllerParseResult result;
        result.error.message = std::string("cannot read the Animator Controller: ") + e.what();
        return result;
    }catch (...){
        ControllerParseResult result;
        result.error.message = "cannot read the Animator Controller";
        return result;
    }
}

std::string anim::writeController(const ControllerData& input){
    ControllerData data = input;
    normalizeController(data, nullptr);
    std::string text = "{\n";
    text += std::string("  \"format\": ") + quote(CONTROLLER_FORMAT) + ",\n";
    text += "  \"version\": " + std::to_string(CONTROLLER_VERSION) + ",\n";
    if (!data.guid.empty()) text += "  \"guid\": " + quote(data.guid) + ",\n";

    if (data.parameters.empty()){
        text += "  \"parameters\": [],\n";
    }else{
        text += "  \"parameters\": [\n";
        for (size_t i = 0; i < data.parameters.size(); i++){
            const ControllerParameter& parameter = data.parameters[i];
            text += "    {\"name\": " + quote(parameter.name) + ", \"type\": " + quote(parameterTypeName(parameter.type));
            switch (parameter.type){
                case ControllerParameterType::Float: text += ", \"default\": " + number(parameter.defaultValue); break;
                case ControllerParameterType::Int: text += ", \"default\": " + number(std::round(parameter.defaultValue)); break;
                case ControllerParameterType::Bool: text += std::string(", \"default\": ") + (parameter.defaultValue != 0.0f ? "true" : "false"); break;
                case ControllerParameterType::Trigger: break;
            }
            text += "}";
            text += (i + 1 < data.parameters.size()) ? ",\n" : "\n";
        }
        text += "  ],\n";
    }

    text += "  \"layers\": [\n";
    for (size_t l = 0; l < data.layers.size(); l++){
        const ControllerLayer& layer = data.layers[l];
        text += "    {\n";
        text += "      \"name\": " + quote(layer.name) + ",\n";
        if (layer.weight != 1.0f) text += "      \"weight\": " + number(layer.weight) + ",\n";
        if (layer.blend != AnimationLayerBlend::Override) text += std::string("      \"blend\": ") + quote(layerBlendName(layer.blend)) + ",\n";
        if (!layer.mask.empty()) text += "      \"mask\": " + quote(layer.mask) + ",\n";
        if (!layer.maskGuid.empty()) text += "      \"mask_guid\": " + quote(layer.maskGuid) + ",\n";
        if (!layer.defaultState.empty()) text += "      \"default_state\": " + quote(layer.defaultState) + ",\n";
        if (layer.hasAnyStatePosition) text += "      \"any_state\": {\"position\": " + positionText(layer.anyStatePosition) + "},\n";
        if (layer.states.empty()){
            text += "      \"states\": [],\n";
        }else{
            text += "      \"states\": [\n";
            for (size_t s = 0; s < layer.states.size(); s++){
                const ControllerState& state = layer.states[s];
                text += "        {\"name\": " + quote(state.name);
                if (!state.clip.empty()){
                    text += ", \"clip\": " + quote(state.clip);
                    if (!state.clipGuid.empty()) text += ", \"clip_guid\": " + quote(state.clipGuid);
                }else if (state.hasBlendTree){
                    const BlendTree1D& tree = state.blendTree;
                    text += ", \"blend_tree\": {\"parameter\": " + quote(tree.parameter);
                    if (tree.automateThresholds) text += ", \"automate_thresholds\": true";
                    text += ", \"children\": [";
                    for (size_t c = 0; c < tree.children.size(); c++){
                        const BlendTreeChild& child = tree.children[c];
                        if (c > 0) text += ", ";
                        text += "{\"clip\": " + quote(child.clip);
                        if (!child.clipGuid.empty()) text += ", \"clip_guid\": " + quote(child.clipGuid);
                        text += ", \"threshold\": " + number(child.threshold);
                        if (child.speed != 1.0f) text += ", \"speed\": " + number(child.speed);
                        if (child.mirror) text += ", \"mirror\": true";
                        text += keptText(child.unknownKeys, childKeys) + "}";
                    }
                    text += "]" + keptText(tree.unknownKeys, treeKeys) + "}";
                }
                if (state.speed != 1.0f) text += ", \"speed\": " + number(state.speed);
                if (!state.speedParameter.empty()) text += ", \"speed_parameter\": " + quote(state.speedParameter);
                if (state.wrap != ClipWrapMode::Default) text += std::string(", \"wrap\": ") + quote(wrapName(state.wrap));
                if (state.cycleOffset != 0.0f) text += ", \"cycle_offset\": " + number(state.cycleOffset);
                if (state.mirror) text += ", \"mirror\": true";
                if (state.footIK) text += ", \"foot_ik\": true";
                if (state.retarget) text += ", \"retarget\": true";
                if (state.hasPosition) text += ", \"position\": " + positionText(state.position);
                text += keptText(state.unknownKeys, stateKeys) + "}";
                text += (s + 1 < layer.states.size()) ? ",\n" : "\n";
            }
            text += "      ],\n";
        }
        if (layer.transitions.empty()){
            text += "      \"transitions\": []";
        }else{
            text += "      \"transitions\": [\n";
            for (size_t t = 0; t < layer.transitions.size(); t++){
                const ControllerTransition& transition = layer.transitions[t];
                text += "        {\"from\": " + quote(transition.from) + ", \"to\": " + quote(transition.to);
                if (!transition.conditions.empty()){
                    text += ", \"conditions\": [";
                    for (size_t c = 0; c < transition.conditions.size(); c++){
                        const ControllerCondition& condition = transition.conditions[c];
                        if (c > 0) text += ", ";
                        text += "{\"parameter\": " + quote(condition.parameter) + ", \"mode\": " + quote(conditionModeName(condition.mode));
                        if (condition.mode != ConditionMode::If && condition.mode != ConditionMode::IfNot){
                            text += ", \"threshold\": " + number(condition.threshold);
                        }
                        text += "}";
                    }
                    text += "]";
                }
                text += std::string(", \"has_exit_time\": ") + (transition.hasExitTime ? "true" : "false");
                text += ", \"exit_time\": " + number(transition.exitTime);
                text += ", \"duration\": " + number(transition.duration);
                text += std::string(", \"fixed_duration\": ") + (transition.fixedDuration ? "true" : "false");
                text += ", \"offset\": " + number(transition.offset);
                if (transition.interruption != TransitionInterruption::None){
                    text += std::string(", \"interruption\": ") + quote(interruptionName(transition.interruption));
                }
                if (transition.from == ANY_STATE){
                    text += std::string(", \"can_transition_to_self\": ") + (transition.canTransitionToSelf ? "true" : "false");
                }
                text += keptText(transition.unknownKeys, transitionKeys) + "}";
                text += (t + 1 < layer.transitions.size()) ? ",\n" : "\n";
            }
            text += "      ]";
        }
        // a layer's unknown keys, one per line
        for (size_t k = 0; k < layer.unknownKeys.size(); k++){
            const auto& entry = layer.unknownKeys[k];
            if (known(layerKeys, entry.first)) continue;
            bool repeated = false;
            for (size_t j = k + 1; j < layer.unknownKeys.size(); j++){
                if (layer.unknownKeys[j].first == entry.first) repeated = true;
            }
            if (repeated) continue;
            text += ",\n      " + quote(entry.first) + ": " + (entry.second.empty() ? std::string("null") : entry.second);
        }
        text += "\n    }";
        text += (l + 1 < data.layers.size()) ? ",\n" : "\n";
    }
    text += "  ],\n";
    text += unknownKeysText(data.unknownKeys, topKeys, sizeof(topKeys) / sizeof(topKeys[0]));
    text.erase(text.size() - 2, 1);
    text += "}\n";
    return text;
}

bool anim::isUnityYamlController(const std::string& text){
    return isUnityYaml(text, "AnimatorController:");
}

const char* anim::unityYamlControllerRefusal(){
    return "This is a Unity .controller (YAML). Create a JasperEngine Animator Controller (Assets > Create > Animator Controller, "
           "or MCP edit_animator {controller, create: {}}).";
}

// ---- validation (spec 5.11) ----

std::vector<ClipWarning> anim::validateController(const ControllerData& data, const ControllerClipResolver& resolve){
    std::vector<ClipWarning> out;
    std::vector<bool> used(data.parameters.size(), false);

    // a parameter reference: exists, and has a type the use takes
    auto checkParameter = [&](const std::string& name, const std::string& path, const char* use,
                              const std::function<bool(ControllerParameterType)>& fits, const std::string& expected){
        const int index = data.findParameter(name);
        if (index < 0){
            out.push_back({path, "no parameter named \"" + name + "\"" + (data.parameters.empty() ? "" : " (add it, or use one of the controller's)")});
            return;
        }
        used[(size_t)index] = true;
        if (!fits(data.parameters[(size_t)index].type)){
            out.push_back({path, std::string(use) + " \"" + name + "\" is a " + parameterTypeName(data.parameters[(size_t)index].type) +
                                 " parameter, expected " + expected});
        }
    };
    auto isNumber = [](ControllerParameterType type){ return type == ControllerParameterType::Float || type == ControllerParameterType::Int; };

    for (size_t l = 0; l < data.layers.size(); l++){
        const ControllerLayer& layer = data.layers[l];
        const std::string path = indexed("", "layers", l);

        // the motions: clips load; does the state loop
        std::vector<bool> loops(layer.states.size(), false);
        for (size_t s = 0; s < layer.states.size(); s++){
            const ControllerState& state = layer.states[s];
            const std::string statePath = indexed(path, "states", s);
            if (!state.speedParameter.empty()){
                checkParameter(state.speedParameter, member(statePath, "speed_parameter"), "speed_parameter",
                               [](ControllerParameterType type){ return type == ControllerParameterType::Float; }, "a float");
            }
            auto stateLoops = [&](const ControllerClipInfo& info){
                if (state.wrap == ClipWrapMode::Loop || state.wrap == ClipWrapMode::PingPong) return true;
                if (state.wrap != ClipWrapMode::Default) return false;
                return info.loops;
            };
            if (state.isEmpty()){
                loops[s] = true;    // an empty state stays as it is: never a dead end
            }else if (!state.clip.empty()){
                if (resolve){
                    ControllerClipInfo info = resolve(state.clip);
                    if (!info.loads) out.push_back({member(statePath, "clip"), "\"" + state.clip + "\" does not load" + (info.error.empty() ? "" : ": " + info.error)});
                    loops[s] = stateLoops(info);
                }else{
                    loops[s] = state.wrap == ClipWrapMode::Loop || state.wrap == ClipWrapMode::PingPong || state.wrap == ClipWrapMode::Default;
                }
            }else{
                const BlendTree1D& tree = state.blendTree;
                const std::string treePath = member(statePath, "blend_tree");
                if (tree.parameter.empty()) out.push_back({member(treePath, "parameter"), "a blend tree needs a float or int parameter"});
                else checkParameter(tree.parameter, member(treePath, "parameter"), "blend tree parameter", isNumber, "a float or an int");
                if (tree.children.empty()) out.push_back({member(treePath, "children"), "the blend tree has no children: the state writes nothing"});
                bool allLoop = !tree.children.empty();
                for (size_t c = 0; c < tree.children.size(); c++){
                    const BlendTreeChild& child = tree.children[c];
                    const std::string childPath = indexed(treePath, "children", c);
                    if (child.clip.empty()){
                        out.push_back({member(childPath, "clip"), "a child without a clip"});
                        allLoop = false;
                        continue;
                    }
                    if (resolve){
                        ControllerClipInfo info = resolve(child.clip);
                        if (!info.loads) out.push_back({member(childPath, "clip"), "\"" + child.clip + "\" does not load" + (info.error.empty() ? "" : ": " + info.error)});
                        allLoop = allLoop && stateLoops(info);
                    }
                }
                loops[s] = resolve ? allLoop : true;
            }
        }

        // transitions: conditions, immediate ones
        std::vector<bool> exits(layer.states.size(), false);
        bool anyExit = false;
        for (size_t t = 0; t < layer.transitions.size(); t++){
            const ControllerTransition& transition = layer.transitions[t];
            const std::string transitionPath = indexed(path, "transitions", t);
            for (size_t c = 0; c < transition.conditions.size(); c++){
                const ControllerCondition& condition = transition.conditions[c];
                const std::string conditionPath = indexed(transitionPath, "conditions", c);
                const int index = data.findParameter(condition.parameter);
                if (index < 0){
                    out.push_back({member(conditionPath, "parameter"), "no parameter named \"" + condition.parameter + "\""});
                    continue;
                }
                used[(size_t)index] = true;
                const ControllerParameterType type = data.parameters[(size_t)index].type;
                if (!conditionModeFits(condition.mode, type)){
                    out.push_back({member(conditionPath, "mode"), std::string("\"") + conditionModeName(condition.mode) + "\" does not fit the " +
                                   parameterTypeName(type) + " parameter \"" + condition.parameter + "\" (" +
                                   (type == ControllerParameterType::Float ? "greater or less" :
                                    type == ControllerParameterType::Int ? "greater, less, equals or not_equal" :
                                    type == ControllerParameterType::Bool ? "if or if_not" : "if") + ")"});
                }
            }
            if (transition.conditions.empty() && !transition.hasExitTime){
                out.push_back({transitionPath, "neither conditions nor an exit time: it fires at once (add a condition or has_exit_time)"});
            }
            if (transition.from == ANY_STATE) anyExit = true;
            else exits[(size_t)layer.findState(transition.from)] = true;
        }

        // dead ends and unreachable states
        const int start = layer.defaultStateIndex();
        std::vector<bool> reached(layer.states.size(), false);
        if (start >= 0){
            std::vector<int> pending = {start};
            reached[(size_t)start] = true;
            for (const ControllerTransition& transition : layer.transitions){
                if (transition.from != ANY_STATE) continue;
                const int to = layer.findState(transition.to);
                if (!reached[(size_t)to]){
                    reached[(size_t)to] = true;
                    pending.push_back(to);
                }
            }
            while (!pending.empty()){
                const int current = pending.back();
                pending.pop_back();
                for (const ControllerTransition& transition : layer.transitions){
                    if (transition.from != layer.states[(size_t)current].name) continue;
                    const int to = layer.findState(transition.to);
                    if (!reached[(size_t)to]){
                        reached[(size_t)to] = true;
                        pending.push_back(to);
                    }
                }
            }
        }
        for (size_t s = 0; s < layer.states.size(); s++){
            const std::string statePath = indexed(path, "states", s);
            if (!reached[s]){
                out.push_back({statePath, "\"" + layer.states[s].name + "\" is unreachable: no transition reaches it from the default state \"" +
                                          layer.states[(size_t)start].name + "\""});
            }
            if (!exits[s] && !anyExit && !loops[s]){
                out.push_back({statePath, "\"" + layer.states[s].name + "\" is a dead end: no transition leaves it and its clip does not loop "
                                          "(it holds its last frame)"});
            }
        }
    }

    for (size_t p = 0; p < data.parameters.size(); p++){
        if (!used[p]){
            out.push_back({indexed("", "parameters", p), "\"" + data.parameters[p].name + "\" is unused: no transition or blend tree reads it"});
        }
    }
    return out;
}

// ---- runtime form ----

std::shared_ptr<const anim::AnimatorController> anim::AnimatorController::build(const ControllerData& source, std::vector<ClipWarning> warnings){
    AnimatorController* controller = new AnimatorController();
    std::shared_ptr<const AnimatorController> shared(controller);
    controller->data = source;
    // already normalized when parsed: what a second pass reports again is not repeated
    std::vector<ClipWarning> again;
    normalizeController(controller->data, &again);
    for (const ClipWarning& warning : again){
        bool known = false;
        for (const ClipWarning& kept : warnings) known |= kept.path == warning.path && kept.message == warning.message;
        if (!known) warnings.push_back(warning);
    }
    controller->warnings = std::move(warnings);
    const ControllerData& data = controller->data;
    controller->layers.resize(data.layers.size());
    for (size_t l = 0; l < data.layers.size(); l++){
        const ControllerLayer& layerData = data.layers[l];
        Layer& layer = controller->layers[l];
        layer.data = &layerData;
        layer.defaultState = layerData.defaultStateIndex();
        layer.states.resize(layerData.states.size());
        for (size_t s = 0; s < layerData.states.size(); s++){
            State& state = layer.states[s];
            state.data = &layerData.states[s];
            state.speedParameter = state.data->speedParameter.empty() ? -1 : data.findParameter(state.data->speedParameter);
            state.blendParameter = state.data->hasBlendTree ? data.findParameter(state.data->blendTree.parameter) : -1;
        }
        for (const ControllerTransition& transitionData : layerData.transitions){
            Transition transition;
            transition.data = &transitionData;
            transition.from = transitionData.from == ANY_STATE ? -1 : layerData.findState(transitionData.from);
            transition.to = layerData.findState(transitionData.to);
            for (const ControllerCondition& condition : transitionData.conditions){
                transition.conditions.push_back({data.findParameter(condition.parameter), condition.mode, condition.threshold});
            }
            const int index = (int)layer.transitions.size();
            layer.transitions.push_back(std::move(transition));
            if (layer.transitions.back().from < 0) layer.anyTransitions.push_back(index);
            else layer.states[(size_t)layer.transitions.back().from].transitions.push_back(index);
        }
    }
    return shared;
}
