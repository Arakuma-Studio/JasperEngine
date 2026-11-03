// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "InputManagerFormat.h"

#include "Input.h"
#include "input/InputActionAssetJson.h"
#include "input/InputControlPath.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>

using namespace jasperengine;

namespace {

    using Json = nlohmann::json;
    // what the engine writes keeps its fields in reading order (name first)
    using OJson = nlohmann::ordered_json;

    // a float as JSON prints it short: 0.001f is 0.001, not 0.0010000000474974513
    double tidy(float value){
        char text[32];
        std::snprintf(text, sizeof(text), "%.6g", static_cast<double>(value));
        return std::strtod(text, nullptr);
    }

    std::string trimmed(const std::string& value){
        size_t start = 0, end = value.size();
        while (start < end && std::isspace(static_cast<unsigned char>(value[start]))) start++;
        while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) end--;
        return value.substr(start, end - start);
    }

    std::string lower(std::string value){
        for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
    }

    bool isYAxis(int axis){
        return axis == D_GAMEPAD_AXIS_LEFT_Y || axis == D_GAMEPAD_AXIS_RIGHT_Y;
    }

    // ---- an .inputactions binding as JSON (kind: binding)

    InputJson bindingDescJson(const InputBindingDesc& binding){
        InputActionAssetDesc asset;
        InputActionMapDesc map;
        map.name = "m";
        InputActionDesc action;
        action.name = "a";
        action.bindings.push_back(binding);
        map.actions.push_back(action);
        asset.maps.push_back(map);
        InputJson json = inputActionAssetToJson(asset)["maps"][0]["actions"][0]["bindings"][0];
        json.erase("id");
        if (json.contains("parts")){
            for (auto it = json["parts"].begin(); it != json["parts"].end(); ++it){
                if (it.value().is_array()){
                    for (InputJson& part : it.value()) part.erase("id");
                }else{
                    it.value().erase("id");
                }
            }
        }
        return json;
    }

    bool bindingDescFromJson(const Json& json, InputBindingDesc& out, std::string& error){
        InputJson asset = InputJson::object();
        asset["version"] = 1;
        InputJson action = InputJson::object();
        action["name"] = "a";
        action["bindings"] = InputJson::array({InputJson::parse(json.dump())});
        InputJson map = InputJson::object();
        map["name"] = "m";
        map["actions"] = InputJson::array({action});
        asset["maps"] = InputJson::array({map});
        InputActionAssetDesc parsed;
        if (!inputActionAssetFromJson(asset, parsed, error))
            return false;
        out = parsed.maps[0].actions[0].bindings[0];
        return true;
    }

    // ---- the flat JSON

    std::string controlName(const InputControl& control){
        return InputControlNames::name(control);
    }

    OJson bindingToJson(const InputManagerBinding& binding){
        OJson j = OJson::object();
        j["kind"] = InputManagerFormat::kindName(binding.kind);
        switch (binding.kind){
            case InputManagerBindingKind::Buttons: {
                OJson positive = OJson::array();
                for (const InputControl& control : binding.positive) positive.push_back(controlName(control));
                j["positive"] = positive;
                if (!binding.negative.empty()){
                    OJson negative = OJson::array();
                    for (const InputControl& control : binding.negative) negative.push_back(controlName(control));
                    j["negative"] = negative;
                }
                if (binding.gamepad >= 0) j["gamepad"] = binding.gamepad;
                j["gravity"] = tidy(binding.gravity);
                j["sensitivity"] = tidy(binding.sensitivity);
                j["dead"] = tidy(binding.dead);
                if (binding.snap) j["snap"] = true;
                break;
            }
            case InputManagerBindingKind::GamepadAxis:
                j["axis"] = InputControlNames::gamepadAxisName(binding.axis);
                if (binding.gamepad >= 0) j["gamepad"] = binding.gamepad;
                else j["gamepad"] = "any";
                j["sensitivity"] = tidy(binding.sensitivity);
                j["dead"] = tidy(binding.dead);
                break;
            case InputManagerBindingKind::MouseDelta:
                j["axis"] = InputManagerFormat::mouseAxisName(binding.mouseAxis);
                j["sensitivity"] = tidy(binding.sensitivity);
                break;
            case InputManagerBindingKind::Binding:
                j["binding"] = OJson::parse(binding.binding.empty() ? std::string("{}") : binding.binding);
                return j;
        }
        if (binding.invert) j["invert"] = true;
        return j;
    }

    OJson actionToJson(const InputManagerAction& action){
        OJson j = OJson::object();
        j["name"] = action.name;
        j["type"] = InputManagerFormat::typeName(action.type);
        if (!action.description.empty()) j["description"] = action.description;
        if (action.pressPoint != 0.5f) j["press_point"] = tidy(action.pressPoint);
        if (action.holdTime != 0.0f) j["hold_time"] = tidy(action.holdTime);
        if (action.deadzone) j["deadzone"] = tidy(*action.deadzone);
        OJson bindings = OJson::array();
        for (const InputManagerBinding& binding : action.bindings) bindings.push_back(bindingToJson(binding));
        j["bindings"] = bindings;
        return j;
    }

    bool readNumber(const Json& j, const char* key, float& out, const std::string& where, std::string& error){
        if (!j.contains(key)) return true;
        const Json& value = j[key];
        if (!value.is_number()){
            error = where + ": " + key + " must be a number";
            return false;
        }
        out = static_cast<float>(value.get<double>());
        return true;
    }

    bool readBool(const Json& j, const char* key, bool& out, const std::string& where, std::string& error){
        if (!j.contains(key)) return true;
        if (!j[key].is_boolean()){
            error = where + ": " + key + " must be true or false";
            return false;
        }
        out = j[key].get<bool>();
        return true;
    }

    bool readGamepad(const Json& j, int& out, const std::string& where, std::string& error){
        if (!j.contains("gamepad")) return true;
        const Json& value = j["gamepad"];
        if (value.is_string() && lower(value.get<std::string>()) == "any"){
            out = -1;
            return true;
        }
        if (value.is_number_integer() && value.get<long long>() >= -1 && value.get<long long>() <= 15){
            out = value.get<int>();
            return true;
        }
        error = where + ": gamepad must be \"any\" or 0-15";
        return false;
    }

    bool readControls(const Json& j, const char* key, std::vector<InputControl>& out, const std::string& where, std::string& error){
        out.clear();
        if (!j.contains(key)) return true;
        Json list = j[key];
        if (list.is_string()) list = Json::array({list});
        if (!list.is_array()){
            error = where + ": " + key + " must be a list of control names";
            return false;
        }
        for (const Json& item : list){
            // "0".."9" may arrive as numbers (YAML, a hand-written file)
            std::string name;
            if (item.is_string()) name = item.get<std::string>();
            else if (item.is_number_integer() && item.get<long long>() >= 0 && item.get<long long>() <= 9) name = std::to_string(item.get<int>());
            else{
                error = where + ": " + key + " must be a list of control names";
                return false;
            }
            InputControl control;
            std::string why;
            if (!InputControlNames::parseButton(name, control, &why)){
                error = where + ": " + why;
                return false;
            }
            if (std::find(out.begin(), out.end(), control) == out.end()) out.push_back(control);
        }
        return true;
    }

    bool checkKeys(const Json& j, const std::set<std::string>& allowed, const std::string& where, std::string& error){
        for (auto it = j.begin(); it != j.end(); ++it){
            if (!allowed.count(it.key())){
                std::string list;
                for (const std::string& key : allowed) list += (list.empty() ? "" : ", ") + key;
                error = where + ": unknown field \"" + it.key() + "\" (fields: " + list + ")";
                return false;
            }
        }
        return true;
    }

    bool bindingFromJson(const Json& j, InputManagerBinding& out, const std::string& where, std::string& error){
        if (!j.is_object()){
            error = where + ": a binding is an object {kind, ...}";
            return false;
        }
        if (!j.contains("kind") || !j["kind"].is_string()){
            error = where + ": kind must be buttons, gamepad_axis, mouse_delta or binding";
            return false;
        }
        const std::string kind = lower(j["kind"].get<std::string>());
        if (kind == "buttons" || kind == "button" || kind == "key") out = InputManagerFormat::makeBinding(InputManagerBindingKind::Buttons);
        else if (kind == "gamepad_axis" || kind == "joystick_axis") out = InputManagerFormat::makeBinding(InputManagerBindingKind::GamepadAxis);
        else if (kind == "mouse_delta" || kind == "mouse_movement") out = InputManagerFormat::makeBinding(InputManagerBindingKind::MouseDelta);
        else if (kind == "binding") out = InputManagerFormat::makeBinding(InputManagerBindingKind::Binding);
        else{
            error = where + ": kind must be buttons, gamepad_axis, mouse_delta or binding, not \"" + kind + "\"";
            return false;
        }

        if (out.kind == InputManagerBindingKind::Binding){
            if (!checkKeys(j, {"kind", "binding"}, where, error)) return false;
            if (!j.contains("binding") || !j["binding"].is_object()){
                error = where + ": binding must be an .inputactions binding {path, ...} or {composite, parts, ...}";
                return false;
            }
            InputBindingDesc desc;
            std::string why;
            if (!bindingDescFromJson(j["binding"], desc, why)){
                error = where + ": " + why;
                return false;
            }
            if (!desc.isComposite() && !InputControlPath::parse(desc.path).isValid()){
                error = where + ": \"" + desc.path + "\" is not a control path";
                return false;
            }
            out.binding = bindingDescJson(desc).dump();
            return true;
        }
        if (out.kind == InputManagerBindingKind::Buttons){
            if (!checkKeys(j, {"kind", "positive", "negative", "gamepad", "gravity", "sensitivity", "dead", "snap", "invert"}, where, error)) return false;
            if (!readControls(j, "positive", out.positive, where, error)) return false;
            if (!readControls(j, "negative", out.negative, where, error)) return false;
            if (!readGamepad(j, out.gamepad, where, error)) return false;
            if (!readNumber(j, "gravity", out.gravity, where, error)) return false;
            if (!readBool(j, "snap", out.snap, where, error)) return false;
        }else if (out.kind == InputManagerBindingKind::GamepadAxis){
            // Unity's gravity and snap mean nothing to a joystick axis: accepted, not kept
            if (!checkKeys(j, {"kind", "axis", "gamepad", "sensitivity", "dead", "invert", "gravity", "snap"}, where, error)) return false;
            if (!j.contains("axis") || !j["axis"].is_string() || InputControlNames::parseGamepadAxis(j["axis"].get<std::string>()) < 0){
                error = where + ": axis must be left_x, left_y, right_x, right_y, left_trigger or right_trigger";
                return false;
            }
            out.axis = InputControlNames::parseGamepadAxis(j["axis"].get<std::string>());
            if (!readGamepad(j, out.gamepad, where, error)) return false;
        }else{
            if (!checkKeys(j, {"kind", "axis", "sensitivity", "invert", "gravity", "dead", "snap"}, where, error)) return false;
            const std::string axis = j.contains("axis") && j["axis"].is_string() ? lower(j["axis"].get<std::string>()) : "";
            if (axis == "x") out.mouseAxis = MouseDeltaAxis::X;
            else if (axis == "y") out.mouseAxis = MouseDeltaAxis::Y;
            else if (axis == "wheel" || axis == "scroll" || axis == "scroll_wheel") out.mouseAxis = MouseDeltaAxis::Wheel;
            else{
                error = where + ": axis must be x, y or wheel";
                return false;
            }
        }
        if (!readNumber(j, "sensitivity", out.sensitivity, where, error)) return false;
        if (out.kind != InputManagerBindingKind::MouseDelta && !readNumber(j, "dead", out.dead, where, error)) return false;
        if (!readBool(j, "invert", out.invert, where, error)) return false;
        return true;
    }

    bool actionFromJson(const Json& j, InputManagerAction& out, size_t index, std::string& error){
        const std::string fallback = "action " + std::to_string(index + 1);
        if (!j.is_object()){
            error = fallback + ": an action is an object {name, type, bindings}";
            return false;
        }
        if (!j.contains("name") || !j["name"].is_string()){
            error = fallback + ": name must be a string";
            return false;
        }
        out = InputManagerAction();
        out.name = trimmed(j["name"].get<std::string>());
        const std::string where = "action \"" + out.name + "\"";
        if (!checkKeys(j, {"name", "type", "description", "press_point", "hold_time", "deadzone", "bindings"}, where, error)) return false;
        if (j.contains("type")){
            const std::string type = j["type"].is_string() ? lower(j["type"].get<std::string>()) : "";
            if (type == "axis") out.type = InputManagerActionType::Axis;
            else if (type == "button") out.type = InputManagerActionType::Button;
            else if (type == "vector2") out.type = InputManagerActionType::Vector2;
            else{
                error = where + ": type must be axis, button or vector2";
                return false;
            }
        }
        if (j.contains("description")){
            if (!j["description"].is_string()){
                error = where + ": description must be a string";
                return false;
            }
            out.description = j["description"].get<std::string>();
        }
        if (!readNumber(j, "press_point", out.pressPoint, where, error)) return false;
        if (!readNumber(j, "hold_time", out.holdTime, where, error)) return false;
        if (j.contains("deadzone")){
            float deadzone = 0.0f;
            if (!readNumber(j, "deadzone", deadzone, where, error)) return false;
            out.deadzone = deadzone;
        }
        if (j.contains("bindings")){
            if (!j["bindings"].is_array()){
                error = where + ": bindings must be a list";
                return false;
            }
            for (size_t b = 0; b < j["bindings"].size(); b++){
                InputManagerBinding binding;
                if (!bindingFromJson(j["bindings"][b], binding, where + " binding " + std::to_string(b + 1), error)) return false;
                out.bindings.push_back(binding);
            }
        }
        return true;
    }

    bool parseJsonText(const std::string& text, Json& out, std::string& error){
        out = Json::parse(text, nullptr, false);
        if (out.is_discarded()){
            error = "not valid JSON";
            return false;
        }
        return true;
    }

    bool validNumber(float value){
        return std::isfinite(value) && value >= 0.0f;
    }

    // ---- ids: derived from where the binding is, so a conversion gives the same asset each time

    std::string idFor(const std::string& seed){
        // FNV-1a
        uint32_t hash = 2166136261u;
        for (unsigned char c : seed){
            hash ^= c;
            hash *= 16777619u;
        }
        char text[9];
        std::snprintf(text, sizeof(text), "%08x", hash);
        return text;
    }

    std::string controlPath(const InputControl& control){
        switch (control.device){
            case InputDevice::Key: return "keyboard/" + InputControlNames::keyName(control.code);
            case InputDevice::Mouse: return "mouse/" + InputControlNames::mouseButtonName(control.code);
            case InputDevice::GamepadButton: return "gamepad/" + InputControlNames::gamepadButtonName(control.code);
            case InputDevice::GamepadAxis: return "gamepad/" + InputControlNames::gamepadAxisName(control.code);
        }
        return "";
    }

    // a part of an Input Manager buttons composite: a plain key, mouse or pad button path
    bool partControl(const InputBindingDesc& part, InputControl& out){
        if (part.scale != 1.0f || part.invert || part.deadzone || part.requiredMods != 0 || part.forbiddenMods != 0 ||
                !part.passthrough.empty())
            return false;
        const InputControlPath path = InputControlPath::parse(part.path);
        switch (path.kind){
            case InputControlKind::KEY: out = {InputDevice::Key, path.index}; break;
            case InputControlKind::MOUSE_BUTTON: out = {InputDevice::Mouse, path.index}; break;
            case InputControlKind::GAMEPAD_BUTTON: out = {InputDevice::GamepadButton, path.index}; break;
            default: return false;
        }
        // the path names it as the Input Manager does
        return controlPath(out) == path.toString() && !InputControlNames::name(out).empty();
    }

    bool plainBinding(const InputBindingDesc& binding){
        return !binding.isComposite() && binding.requiredMods == 0 && binding.forbiddenMods == 0 && binding.groups.empty() &&
               binding.passthrough.empty() && !binding.smoothing;
    }

}

// ---- defaults

InputManagerBinding InputManagerFormat::makeBinding(InputManagerBindingKind kind){
    InputManagerBinding binding;
    binding.kind = kind;
    if (kind == InputManagerBindingKind::GamepadAxis){
        binding.gravity = 0.0f;
        binding.sensitivity = 1.0f;
        binding.dead = 0.19f;
    }else if (kind == InputManagerBindingKind::MouseDelta){
        binding.gravity = 0.0f;
        binding.sensitivity = 0.1f;
        binding.dead = 0.0f;
    }else if (kind == InputManagerBindingKind::Binding){
        binding.gravity = 0.0f;
        binding.sensitivity = 0.0f;
        binding.dead = 0.0f;
    }
    return binding;
}

std::vector<InputManagerAction> InputManagerFormat::defaults(){
    auto key = [](int code){ return InputControl{InputDevice::Key, code}; };
    auto mouse = [](int code){ return InputControl{InputDevice::Mouse, code}; };
    auto pad = [](int code){ return InputControl{InputDevice::GamepadButton, code}; };

    auto keys = [](std::vector<InputControl> negative, std::vector<InputControl> positive){
        InputManagerBinding binding = makeBinding(InputManagerBindingKind::Buttons);
        binding.negative = std::move(negative);
        binding.positive = std::move(positive);
        binding.snap = true;
        return binding;
    };
    auto stick = [](int axis, bool invert){
        InputManagerBinding binding = makeBinding(InputManagerBindingKind::GamepadAxis);
        binding.axis = axis;
        binding.invert = invert;
        return binding;
    };
    // Unity's buttons: gravity and sensitivity 1000 (instant)
    auto button = [](std::vector<InputControl> positive){
        InputManagerBinding binding = makeBinding(InputManagerBindingKind::Buttons);
        binding.positive = std::move(positive);
        binding.gravity = 1000.0f;
        binding.sensitivity = 1000.0f;
        return binding;
    };
    auto mouseMove = [](MouseDeltaAxis axis){
        InputManagerBinding binding = makeBinding(InputManagerBindingKind::MouseDelta);
        binding.mouseAxis = axis;
        return binding;
    };
    auto action = [](const char* name, InputManagerActionType type, const char* description, std::vector<InputManagerBinding> bindings){
        InputManagerAction a;
        a.name = name;
        a.type = type;
        a.description = description;
        a.bindings = std::move(bindings);
        return a;
    };

    return {
        action("Horizontal", InputManagerActionType::Axis, "Move left / right",
               {keys({key(D_KEY_LEFT), key(D_KEY_A)}, {key(D_KEY_RIGHT), key(D_KEY_D)}), stick(D_GAMEPAD_AXIS_LEFT_X, false)}),
        action("Vertical", InputManagerActionType::Axis, "Move down / up",
               {keys({key(D_KEY_DOWN), key(D_KEY_S)}, {key(D_KEY_UP), key(D_KEY_W)}), stick(D_GAMEPAD_AXIS_LEFT_Y, true)}),
        action("Fire1", InputManagerActionType::Button, "", {button({key(D_KEY_LEFT_CONTROL), mouse(D_MOUSE_BUTTON_LEFT)}), button({pad(D_GAMEPAD_BUTTON_A)})}),
        action("Fire2", InputManagerActionType::Button, "", {button({key(D_KEY_LEFT_ALT), mouse(D_MOUSE_BUTTON_RIGHT)}), button({pad(D_GAMEPAD_BUTTON_B)})}),
        action("Fire3", InputManagerActionType::Button, "", {button({key(D_KEY_LEFT_SHIFT), mouse(D_MOUSE_BUTTON_MIDDLE)}), button({pad(D_GAMEPAD_BUTTON_X)})}),
        action("Jump", InputManagerActionType::Button, "", {button({key(D_KEY_SPACE)}), button({pad(D_GAMEPAD_BUTTON_Y)})}),
        action("Mouse X", InputManagerActionType::Axis, "Mouse movement, right positive", {mouseMove(MouseDeltaAxis::X)}),
        action("Mouse Y", InputManagerActionType::Axis, "Mouse movement, up positive", {mouseMove(MouseDeltaAxis::Y)}),
        action("Mouse ScrollWheel", InputManagerActionType::Axis, "Mouse wheel, up positive", {mouseMove(MouseDeltaAxis::Wheel)}),
        action("Submit", InputManagerActionType::Button, "", {button({key(D_KEY_ENTER), key(D_KEY_KP_ENTER), key(D_KEY_SPACE)}), button({pad(D_GAMEPAD_BUTTON_A)})}),
        action("Cancel", InputManagerActionType::Button, "", {button({key(D_KEY_ESCAPE)}), button({pad(D_GAMEPAD_BUTTON_B)})}),
    };
}

InputActionAssetDesc InputManagerFormat::defaultsAsset(){
    InputActionAssetDesc asset;
    asset.maps.push_back(toMap(defaults()));
    return asset;
}

// ---- JSON

bool InputManagerFormat::parseActionsJson(const std::string& json, std::vector<InputManagerAction>& out, std::string* error){
    std::string why;
    Json j;
    if (!parseJsonText(json, j, why)){
        if (error) *error = why;
        return false;
    }
    const Json* list = &j;
    if (j.is_object()){
        if (j.contains("version") && (!j["version"].is_number_integer() || j["version"].get<int>() != 1)){
            if (error) *error = "input version " + j["version"].dump() + " is not supported (this engine reads version 1)";
            return false;
        }
        for (auto it = j.begin(); it != j.end(); ++it){
            if (it.key() != "version" && it.key() != "actions"){
                if (error) *error = "unknown field \"" + it.key() + "\" (fields: version, actions)";
                return false;
            }
        }
        if (!j.contains("actions")){
            if (error) *error = "the input definition has no actions list";
            return false;
        }
        list = &j["actions"];
    }
    if (!list->is_array()){
        if (error) *error = "actions must be a list";
        return false;
    }
    std::vector<InputManagerAction> actions;
    for (size_t i = 0; i < list->size(); i++){
        InputManagerAction action;
        if (!actionFromJson((*list)[i], action, i, why)){
            if (error) *error = why;
            return false;
        }
        actions.push_back(action);
    }
    why = validate(actions);
    if (!why.empty()){
        if (error) *error = why;
        return false;
    }
    out = std::move(actions);
    return true;
}

std::string InputManagerFormat::actionsToJson(const std::vector<InputManagerAction>& actions, int indent){
    OJson list = OJson::array();
    for (const InputManagerAction& action : actions) list.push_back(actionToJson(action));
    OJson j = OJson::object();
    j["version"] = 1;
    j["actions"] = list;
    return j.dump(indent);
}

bool InputManagerFormat::parseBindingsJson(const std::string& json, std::vector<InputManagerBinding>& out, std::string* error,
                                           const std::string& action){
    std::string why;
    Json j;
    if (!parseJsonText(json, j, why)){
        if (error) *error = why;
        return false;
    }
    if (!j.is_array()){
        if (error) *error = "bindings must be a list";
        return false;
    }
    const std::string where = action.empty() ? std::string("binding") : "action \"" + action + "\" binding";
    std::vector<InputManagerBinding> bindings;
    for (size_t b = 0; b < j.size(); b++){
        InputManagerBinding binding;
        if (!bindingFromJson(j[b], binding, where + " " + std::to_string(b + 1), why)){
            if (error) *error = why;
            return false;
        }
        bindings.push_back(binding);
    }
    why = validateBindings(bindings, action);
    if (!why.empty()){
        if (error) *error = why;
        return false;
    }
    out = std::move(bindings);
    return true;
}

std::string InputManagerFormat::bindingsToJson(const std::vector<InputManagerBinding>& bindings){
    OJson list = OJson::array();
    for (const InputManagerBinding& binding : bindings) list.push_back(bindingToJson(binding));
    return list.dump();
}

std::string InputManagerFormat::validateBindings(const std::vector<InputManagerBinding>& bindings, const std::string& action){
    const std::string prefix = action.empty() ? std::string("binding ") : "action \"" + action + "\" binding ";
    for (size_t b = 0; b < bindings.size(); b++){
        const InputManagerBinding& binding = bindings[b];
        const std::string where = prefix + std::to_string(b + 1);
        if (binding.kind == InputManagerBindingKind::Binding){
            InputBindingDesc desc;
            std::string why;
            const Json json = Json::parse(binding.binding.empty() ? std::string("null") : binding.binding, nullptr, false);
            if (!json.is_object() || !bindingDescFromJson(json, desc, why))
                return where + ": not an .inputactions binding" + (why.empty() ? std::string() : " (" + why + ")");
            continue;
        }
        if (!validNumber(binding.gravity) || !validNumber(binding.sensitivity) || !validNumber(binding.dead))
            return where + ": gravity, sensitivity and dead must be finite numbers >= 0";
        if (binding.dead >= 1.0f)
            return where + ": dead must be below 1";
        if (binding.gamepad < -1 || binding.gamepad > 15)
            return where + ": gamepad must be any (-1) or 0-15";
        if (binding.kind == InputManagerBindingKind::Buttons){
            for (const std::vector<InputControl>* list : {&binding.positive, &binding.negative}){
                for (const InputControl& control : *list){
                    if (control.device == InputDevice::GamepadAxis || InputControlNames::name(control).empty())
                        return where + ": a buttons binding lists keys, mouse buttons and gamepad buttons";
                }
            }
        }else if (binding.kind == InputManagerBindingKind::GamepadAxis){
            if (binding.axis < 0 || binding.axis > D_GAMEPAD_AXIS_LAST)
                return where + ": unknown gamepad axis";
        }
    }
    return "";
}

std::string InputManagerFormat::validate(const std::vector<InputManagerAction>& actions){
    std::set<std::string> names;
    for (size_t i = 0; i < actions.size(); i++){
        const InputManagerAction& action = actions[i];
        if (trimmed(action.name).empty())
            return "action " + std::to_string(i + 1) + " has no name";
        if (trimmed(action.name) != action.name)
            return "action \"" + action.name + "\": the name has leading or trailing spaces";
        if (!names.insert(action.name).second)
            return "two actions are named \"" + action.name + "\" (names are unique, case-sensitive)";
        if (!std::isfinite(action.pressPoint) || action.pressPoint <= 0.0f || action.pressPoint > 1.0f)
            return "action \"" + action.name + "\": press_point must be above 0 and at most 1";
        if (!validNumber(action.holdTime))
            return "action \"" + action.name + "\": hold_time must be a finite number >= 0";
        if (action.deadzone && (!validNumber(*action.deadzone) || *action.deadzone >= 1.0f))
            return "action \"" + action.name + "\": deadzone must be a number from 0 to below 1";
        const std::string problem = validateBindings(action.bindings, action.name);
        if (!problem.empty()) return problem;
    }
    return "";
}

const char* InputManagerFormat::typeName(InputManagerActionType type){
    switch (type){
        case InputManagerActionType::Button: return "button";
        case InputManagerActionType::Vector2: return "vector2";
        default: return "axis";
    }
}

const char* InputManagerFormat::kindName(InputManagerBindingKind kind){
    switch (kind){
        case InputManagerBindingKind::GamepadAxis: return "gamepad_axis";
        case InputManagerBindingKind::MouseDelta: return "mouse_delta";
        case InputManagerBindingKind::Binding: return "binding";
        default: return "buttons";
    }
}

const char* InputManagerFormat::mouseAxisName(MouseDeltaAxis axis){
    switch (axis){
        case MouseDeltaAxis::Y: return "y";
        case MouseDeltaAxis::Wheel: return "wheel";
        default: return "x";
    }
}

// ---- the Input Manager map

InputBindingDesc InputManagerFormat::toBinding(const InputManagerBinding& binding, const std::string& idSeed){
    InputBindingDesc desc;
    switch (binding.kind){
        case InputManagerBindingKind::Buttons: {
            // the lists are the roles of an axis1d composite, the knobs its smoothing
            desc.composite = InputCompositeType::AXIS_1D;
            int part = 0;
            for (const InputControl& control : binding.negative){
                InputBindingDesc one;
                one.path = controlPath(control);
                one.id = idFor(idSeed + "/negative/" + std::to_string(part++));
                desc.parts.emplace_back("negative", one);
            }
            part = 0;
            for (const InputControl& control : binding.positive){
                InputBindingDesc one;
                one.path = controlPath(control);
                one.id = idFor(idSeed + "/positive/" + std::to_string(part++));
                desc.parts.emplace_back("positive", one);
            }
            InputSmoothing smoothing;
            smoothing.gravity = binding.gravity;
            smoothing.sensitivity = binding.sensitivity;
            smoothing.dead = binding.dead;
            smoothing.snap = binding.snap;
            desc.smoothing = smoothing;
            desc.invert = binding.invert;
            desc.gamepad = binding.gamepad;
            break;
        }
        case InputManagerBindingKind::GamepadAxis:
            // the action's y is up-positive (the Input Manager's reads the pad's, up negative)
            desc.path = "gamepad/" + InputControlNames::gamepadAxisName(binding.axis);
            desc.deadzone = binding.dead;
            desc.scale = binding.sensitivity;
            desc.invert = isYAxis(binding.axis) ? !binding.invert : binding.invert;
            desc.gamepad = binding.gamepad;
            break;
        case InputManagerBindingKind::MouseDelta:
            // the canvas y grows down: Mouse Y up-positive is mouse/delta/y inverted
            desc.path = binding.mouseAxis == MouseDeltaAxis::X ? "mouse/delta/x" : binding.mouseAxis == MouseDeltaAxis::Y ? "mouse/delta/y" : "mouse/scroll/y";
            desc.scale = binding.sensitivity;
            desc.invert = binding.mouseAxis == MouseDeltaAxis::Y ? !binding.invert : binding.invert;
            break;
        case InputManagerBindingKind::Binding: {
            std::string error;
            const Json json = Json::parse(binding.binding.empty() ? std::string("{}") : binding.binding, nullptr, false);
            if (json.is_object())
                bindingDescFromJson(json, desc, error);
            int part = 0;
            for (auto& one : desc.parts)
                one.second.id = idFor(idSeed + "/" + one.first + "/" + std::to_string(part++));
            break;
        }
    }
    desc.id = idFor(idSeed);
    return desc;
}

InputManagerBinding InputManagerFormat::fromBinding(const InputBindingDesc& desc){
    // buttons: an axis1d composite with smoothing whose parts are plain buttons
    if (desc.composite == InputCompositeType::AXIS_1D && desc.smoothing && desc.scale == 1.0f && !desc.deadzone &&
            desc.requiredMods == 0 && desc.forbiddenMods == 0 && desc.groups.empty() && desc.passthrough.empty() && desc.normalize){
        InputManagerBinding binding = makeBinding(InputManagerBindingKind::Buttons);
        bool plain = true;
        for (const auto& [role, part] : desc.parts){
            InputControl control;
            if (!partControl(part, control) || (role != "negative" && role != "positive")){
                plain = false;
                break;
            }
            std::vector<InputControl>& list = role == "negative" ? binding.negative : binding.positive;
            if (std::find(list.begin(), list.end(), control) != list.end()){
                plain = false;
                break;
            }
            list.push_back(control);
        }
        if (plain){
            binding.gravity = desc.smoothing->gravity;
            binding.sensitivity = desc.smoothing->sensitivity;
            binding.dead = desc.smoothing->dead;
            binding.snap = desc.smoothing->snap;
            binding.invert = desc.invert;
            binding.gamepad = desc.gamepad;
            return binding;
        }
    }
    if (plainBinding(desc)){
        const InputControlPath path = InputControlPath::parse(desc.path);
        if (path.kind == InputControlKind::GAMEPAD_AXIS && desc.deadzone && path.toString() == desc.path){
            InputManagerBinding binding = makeBinding(InputManagerBindingKind::GamepadAxis);
            binding.axis = path.index;
            binding.dead = *desc.deadzone;
            binding.sensitivity = desc.scale;
            binding.invert = isYAxis(path.index) ? !desc.invert : desc.invert;
            binding.gamepad = desc.gamepad;
            if (validNumber(binding.sensitivity) && binding.dead >= 0.0f && binding.dead < 1.0f)
                return binding;
        }
        if (!desc.deadzone && desc.gamepad < 0 && (desc.path == "mouse/delta/x" || desc.path == "mouse/delta/y" || desc.path == "mouse/scroll/y") &&
                validNumber(desc.scale)){
            InputManagerBinding binding = makeBinding(InputManagerBindingKind::MouseDelta);
            binding.mouseAxis = desc.path == "mouse/delta/x" ? MouseDeltaAxis::X : desc.path == "mouse/delta/y" ? MouseDeltaAxis::Y : MouseDeltaAxis::Wheel;
            binding.sensitivity = desc.scale;
            binding.invert = binding.mouseAxis == MouseDeltaAxis::Y ? !desc.invert : desc.invert;
            return binding;
        }
    }
    InputManagerBinding binding = makeBinding(InputManagerBindingKind::Binding);
    binding.binding = bindingDescJson(desc).dump();
    return binding;
}

InputActionDesc InputManagerFormat::toAction(const InputManagerAction& action, const std::string& idSeed){
    InputActionDesc desc;
    desc.name = action.name;
    desc.description = action.description;
    desc.type = action.type == InputManagerActionType::Button ? InputActionType::BUTTON
              : action.type == InputManagerActionType::Vector2 ? InputActionType::VECTOR2 : InputActionType::AXIS;
    desc.pressPoint = action.pressPoint;
    desc.holdTime = action.holdTime;
    desc.deadzone = action.deadzone;
    for (size_t b = 0; b < action.bindings.size(); b++)
        desc.bindings.push_back(toBinding(action.bindings[b], idSeed + "/" + std::to_string(b)));
    return desc;
}

InputManagerAction InputManagerFormat::fromAction(const InputActionDesc& desc){
    InputManagerAction action;
    action.name = desc.name;
    action.description = desc.description;
    action.type = desc.type == InputActionType::BUTTON ? InputManagerActionType::Button
                : desc.type == InputActionType::VECTOR2 ? InputManagerActionType::Vector2 : InputManagerActionType::Axis;
    action.pressPoint = desc.pressPoint;
    action.holdTime = desc.holdTime;
    action.deadzone = desc.deadzone;
    for (const InputBindingDesc& binding : desc.bindings)
        action.bindings.push_back(fromBinding(binding));
    return action;
}

InputActionMapDesc InputManagerFormat::toMap(const std::vector<InputManagerAction>& actions, const std::string& mapName){
    InputActionMapDesc map;
    map.name = mapName;
    map.priority = 0;
    map.enabled = true;
    // the Input Manager's actions take no control from other maps; typing and menus win over
    // them (ignoreWhenUIConsumes: a focused text field, gamepad navigation, presses the UI took)
    map.consume = false;
    map.exclusive = false;
    map.ignoreWhenUIConsumes = true;
    std::set<std::string> ids;
    for (const InputManagerAction& action : actions){
        InputActionDesc desc = toAction(action, mapName + "/" + action.name);
        // distinct ids within the map (a hash collision takes the next seed)
        auto unique = [&](InputBindingDesc& binding, const std::string& seed){
            for (int n = 1; !ids.insert(binding.id).second; n++)
                binding.id = idFor(seed + "#" + std::to_string(n));
        };
        for (size_t b = 0; b < desc.bindings.size(); b++){
            const std::string seed = mapName + "/" + action.name + "/" + std::to_string(b);
            unique(desc.bindings[b], seed);
            for (size_t p = 0; p < desc.bindings[b].parts.size(); p++)
                unique(desc.bindings[b].parts[p].second, seed + "/part/" + std::to_string(p));
        }
        map.actions.push_back(std::move(desc));
    }
    return map;
}

std::vector<InputManagerAction> InputManagerFormat::fromMap(const InputActionMapDesc& map){
    std::vector<InputManagerAction> actions;
    for (const InputActionDesc& action : map.actions)
        actions.push_back(fromAction(action));
    return actions;
}

void InputManagerFormat::setInAsset(InputActionAssetDesc& asset, const std::vector<InputManagerAction>& actions){
    InputActionMapDesc map = toMap(actions);
    if (InputActionMapDesc* existing = asset.findMap(MAP_NAME)){
        // the map keeps its own settings (priority, enabled, consume...) and unknown keys
        map.priority = existing->priority;
        map.enabled = existing->enabled;
        map.consume = existing->consume;
        map.exclusive = existing->exclusive;
        map.ignoreWhenUIConsumes = existing->ignoreWhenUIConsumes;
        map.passthrough = existing->passthrough;
        *existing = std::move(map);
    }else{
        asset.maps.push_back(std::move(map));
    }
    // ids unique in the whole asset
    asset.assignMissingIds();
}
