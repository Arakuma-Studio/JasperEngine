// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "InputActions.h"

#include "Log.h"
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <unordered_map>

using namespace jasperengine;

using Json = nlohmann::json;
// what the engine writes keeps its fields in reading order (name first)
using OJson = nlohmann::ordered_json;

namespace {

    // what an action holds besides its definition
    struct ActionRuntime {
        bool enabled = true;
        bool hasOverride = false;
        std::vector<InputBinding> override;
        std::vector<float> smoothed;      // per binding in use (Buttons)
        float boundedValue = 0.0f;        // the bindings' bounded sum (unclamped)
        float boundedRaw = 0.0f;
        float deltaValue = 0.0f;          // the bindings' deltas (mouse movement)
        bool pressed = false;
        bool down = false;
        bool up = false;
    };

    struct Source {
        uintptr_t owner = 0;
        std::string action;
        int index = -1;                   // into the actions, -1 while the name is unknown
        float value = 0.0f;
        bool delta = false;
        InputSourceKind kind = InputSourceKind::Control;
    };

    struct Registry {
        bool loaded = false;
        std::vector<InputAction> actions;
        std::vector<ActionRuntime> runtime;
        std::unordered_map<std::string, int> byName;
        std::vector<Source> sources;
        std::vector<std::string> frameDowns;
        std::vector<std::string> frameUps;
        std::set<std::string> reportedUnknown;
        std::vector<std::string> namedSources;
    };

    Registry& registry() {
        static Registry s;
        return s;
    }

    void rebuildIndex(Registry& s);

    Registry& loaded() {
        Registry& s = registry();
        if (!s.loaded) {
            s.loaded = true;
            s.actions = InputActions::defaults();
            s.runtime.assign(s.actions.size(), ActionRuntime());
            rebuildIndex(s);
        }
        return s;
    }

    void rebuildIndex(Registry& s) {
        s.byName.clear();
        for (size_t i = 0; i < s.actions.size(); i++) s.byName[s.actions[i].name] = static_cast<int>(i);
        for (Source& source : s.sources) {
            auto it = s.byName.find(source.action);
            source.index = it == s.byName.end() ? -1 : it->second;
        }
    }

    // the index of an action, -1 (and one error per name) when there is none
    int find(const std::string& name, bool report = true) {
        Registry& s = loaded();
        auto it = s.byName.find(name);
        if (it != s.byName.end()) return it->second;
        if (report && s.reportedUnknown.insert(name).second) {
            Log::error("Input action \"%s\" does not exist (Project Settings > Input)", name.c_str());
        }
        return -1;
    }

    const std::vector<InputBinding>& bindingsInUse(const Registry& s, int index) {
        const ActionRuntime& runtime = s.runtime[index];
        return runtime.hasOverride ? runtime.override : s.actions[index].bindings;
    }

    // a float as JSON prints it short: 0.001f is 0.001, not 0.0010000000474974513
    double tidy(float value) {
        char text[32];
        snprintf(text, sizeof(text), "%.6g", static_cast<double>(value));
        return std::strtod(text, nullptr);
    }

    float moveTowards(float current, float target, float maxDelta) {
        if (std::fabs(target - current) <= maxDelta) return target;
        return current + (target > current ? maxDelta : -maxDelta);
    }

    float sign(float value) {
        return value > 0.0f ? 1.0f : (value < 0.0f ? -1.0f : 0.0f);
    }

    bool isTrigger(int axis) {
        return axis == D_GAMEPAD_AXIS_LEFT_TRIGGER || axis == D_GAMEPAD_AXIS_RIGHT_TRIGGER;
    }

    // P7: what a focused text field or a document with gamepad focus takes
    bool suppressed(const InputControl& control, const InputSuppression& suppression) {
        if (control.device == InputDevice::Key) return suppression.keyboard;
        if (control.device == InputDevice::GamepadButton && suppression.gamepadNavigation) {
            return control.code == D_GAMEPAD_BUTTON_A || control.code == D_GAMEPAD_BUTTON_DPAD_UP ||
                   control.code == D_GAMEPAD_BUTTON_DPAD_RIGHT || control.code == D_GAMEPAD_BUTTON_DPAD_DOWN ||
                   control.code == D_GAMEPAD_BUTTON_DPAD_LEFT;
        }
        return false;
    }

    // held now, and pressed in this frame (a press and release inside the frame count as a press)
    void readControl(const InputControl& control, int gamepad, bool& held, bool& pressedThisFrame) {
        held = false;
        pressedThisFrame = false;
        switch (control.device) {
            case InputDevice::Key:
                held = Input::isKeyPressed(control.code);
                pressedThisFrame = Input::getKeyDown(control.code);
                break;
            case InputDevice::Mouse:
                held = Input::isMousePressed(control.code);
                pressedThisFrame = Input::getMouseButtonDown(control.code);
                break;
            case InputDevice::GamepadButton:
                if (gamepad < 0) {
                    for (size_t i = 0; i < Input::numGamepads(); i++) {
                        if (Input::isGamepadButtonPressed(Input::getGamepadId(i), control.code)) held = true;
                    }
                } else {
                    held = Input::isGamepadButtonPressed(gamepad, control.code);
                }
                pressedThisFrame = Input::getGamepadButtonDown(gamepad, control.code);
                break;
            case InputDevice::GamepadAxis:
                break;
        }
    }

    // a gamepad axis: the pad's, or (any pad) the one pushed furthest; triggers 0..1
    float readGamepadAxis(int gamepad, int axis) {
        auto read = [axis](int id) {
            const float value = Input::getGamepadAxis(id, axis);
            return isTrigger(axis) ? (value + 1.0f) * 0.5f : value;
        };
        if (gamepad >= 0) return Input::isGamepadConnected(gamepad) ? read(gamepad) : 0.0f;
        float best = 0.0f;
        for (size_t i = 0; i < Input::numGamepads(); i++) {
            const float value = read(Input::getGamepadId(i));
            if (std::fabs(value) > std::fabs(best)) best = value;
        }
        return best;
    }

    // ---- JSON

    std::string controlName(const InputControl& control) {
        return InputControlNames::name(control);
    }

    OJson bindingToJson(const InputBinding& binding) {
        OJson j = OJson::object();
        j["kind"] = InputActions::kindName(binding.kind);
        switch (binding.kind) {
            case InputBindingKind::Buttons: {
                OJson positive = OJson::array();
                for (const InputControl& control : binding.positive) positive.push_back(controlName(control));
                j["positive"] = positive;
                if (!binding.negative.empty()) {
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
            case InputBindingKind::GamepadAxis:
                j["axis"] = InputControlNames::gamepadAxisName(binding.axis);
                if (binding.gamepad >= 0) j["gamepad"] = binding.gamepad;
                else j["gamepad"] = "any";
                j["sensitivity"] = tidy(binding.sensitivity);
                j["dead"] = tidy(binding.dead);
                break;
            case InputBindingKind::MouseDelta:
                j["axis"] = InputActions::mouseAxisName(binding.mouseAxis);
                j["sensitivity"] = tidy(binding.sensitivity);
                break;
        }
        if (binding.invert) j["invert"] = true;
        return j;
    }

    OJson actionToJson(const InputAction& action) {
        OJson j = OJson::object();
        j["name"] = action.name;
        j["type"] = InputActions::typeName(action.type);
        if (!action.description.empty()) j["description"] = action.description;
        if (action.pressPoint != 0.5f) j["press_point"] = tidy(action.pressPoint);
        OJson bindings = OJson::array();
        for (const InputBinding& binding : action.bindings) bindings.push_back(bindingToJson(binding));
        j["bindings"] = bindings;
        return j;
    }

    std::string trimmed(const std::string& value) {
        size_t start = 0, end = value.size();
        while (start < end && std::isspace(static_cast<unsigned char>(value[start]))) start++;
        while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) end--;
        return value.substr(start, end - start);
    }

    std::string lower(std::string value) {
        for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
    }

    bool readNumber(const Json& j, const char* key, float& out, const std::string& where, std::string& error) {
        if (!j.contains(key)) return true;
        const Json& value = j[key];
        if (!value.is_number()) {
            error = where + ": " + key + " must be a number";
            return false;
        }
        out = static_cast<float>(value.get<double>());
        return true;
    }

    bool readBool(const Json& j, const char* key, bool& out, const std::string& where, std::string& error) {
        if (!j.contains(key)) return true;
        if (!j[key].is_boolean()) {
            error = where + ": " + key + " must be true or false";
            return false;
        }
        out = j[key].get<bool>();
        return true;
    }

    bool readGamepad(const Json& j, int& out, const std::string& where, std::string& error) {
        if (!j.contains("gamepad")) return true;
        const Json& value = j["gamepad"];
        if (value.is_string() && lower(value.get<std::string>()) == "any") {
            out = -1;
            return true;
        }
        if (value.is_number_integer() && value.get<long long>() >= -1 && value.get<long long>() <= 15) {
            out = value.get<int>();
            return true;
        }
        error = where + ": gamepad must be \"any\" or 0-15";
        return false;
    }

    bool readControls(const Json& j, const char* key, std::vector<InputControl>& out, const std::string& where,
                      std::string& error) {
        out.clear();
        if (!j.contains(key)) return true;
        Json list = j[key];
        if (list.is_string()) list = Json::array({list});
        if (!list.is_array()) {
            error = where + ": " + key + " must be a list of control names";
            return false;
        }
        for (const Json& item : list) {
            // "0".."9" may arrive as numbers (YAML, a hand-written file)
            std::string name;
            if (item.is_string()) name = item.get<std::string>();
            else if (item.is_number_integer() && item.get<long long>() >= 0 && item.get<long long>() <= 9) name = std::to_string(item.get<int>());
            else {
                error = where + ": " + key + " must be a list of control names";
                return false;
            }
            InputControl control;
            std::string why;
            if (!InputControlNames::parseButton(name, control, &why)) {
                error = where + ": " + why;
                return false;
            }
            if (std::find(out.begin(), out.end(), control) == out.end()) out.push_back(control);
        }
        return true;
    }

    bool checkKeys(const Json& j, const std::set<std::string>& allowed, const std::string& where, std::string& error) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (!allowed.count(it.key())) {
                std::string list;
                for (const std::string& key : allowed) list += (list.empty() ? "" : ", ") + key;
                error = where + ": unknown field \"" + it.key() + "\" (fields: " + list + ")";
                return false;
            }
        }
        return true;
    }

    bool bindingFromJson(const Json& j, InputBinding& out, const std::string& where, std::string& error) {
        if (!j.is_object()) {
            error = where + ": a binding is an object {kind, ...}";
            return false;
        }
        if (!j.contains("kind") || !j["kind"].is_string()) {
            error = where + ": kind must be buttons, gamepad_axis or mouse_delta";
            return false;
        }
        const std::string kind = lower(j["kind"].get<std::string>());
        if (kind == "buttons" || kind == "button" || kind == "key") out = InputActions::makeBinding(InputBindingKind::Buttons);
        else if (kind == "gamepad_axis" || kind == "joystick_axis") out = InputActions::makeBinding(InputBindingKind::GamepadAxis);
        else if (kind == "mouse_delta" || kind == "mouse_movement") out = InputActions::makeBinding(InputBindingKind::MouseDelta);
        else {
            error = where + ": kind must be buttons, gamepad_axis or mouse_delta, not \"" + kind + "\"";
            return false;
        }

        if (out.kind == InputBindingKind::Buttons) {
            if (!checkKeys(j, {"kind", "positive", "negative", "gamepad", "gravity", "sensitivity", "dead", "snap", "invert"}, where, error)) return false;
            if (!readControls(j, "positive", out.positive, where, error)) return false;
            if (!readControls(j, "negative", out.negative, where, error)) return false;
            if (!readGamepad(j, out.gamepad, where, error)) return false;
            if (!readNumber(j, "gravity", out.gravity, where, error)) return false;
            if (!readBool(j, "snap", out.snap, where, error)) return false;
        } else if (out.kind == InputBindingKind::GamepadAxis) {
            // Unity's gravity and snap mean nothing to a joystick axis: accepted, not kept
            if (!checkKeys(j, {"kind", "axis", "gamepad", "sensitivity", "dead", "invert", "gravity", "snap"}, where, error)) return false;
            if (!j.contains("axis") || !j["axis"].is_string() || InputControlNames::parseGamepadAxis(j["axis"].get<std::string>()) < 0) {
                error = where + ": axis must be left_x, left_y, right_x, right_y, left_trigger or right_trigger";
                return false;
            }
            out.axis = InputControlNames::parseGamepadAxis(j["axis"].get<std::string>());
            if (!readGamepad(j, out.gamepad, where, error)) return false;
        } else {
            if (!checkKeys(j, {"kind", "axis", "sensitivity", "invert", "gravity", "dead", "snap"}, where, error)) return false;
            const std::string axis = j.contains("axis") && j["axis"].is_string() ? lower(j["axis"].get<std::string>()) : "";
            if (axis == "x") out.mouseAxis = MouseDeltaAxis::X;
            else if (axis == "y") out.mouseAxis = MouseDeltaAxis::Y;
            else if (axis == "wheel" || axis == "scroll" || axis == "scroll_wheel") out.mouseAxis = MouseDeltaAxis::Wheel;
            else {
                error = where + ": axis must be x, y or wheel";
                return false;
            }
        }
        if (!readNumber(j, "sensitivity", out.sensitivity, where, error)) return false;
        if (out.kind != InputBindingKind::MouseDelta && !readNumber(j, "dead", out.dead, where, error)) return false;
        if (!readBool(j, "invert", out.invert, where, error)) return false;
        return true;
    }

    bool actionFromJson(const Json& j, InputAction& out, size_t index, std::string& error) {
        const std::string fallback = "action " + std::to_string(index + 1);
        if (!j.is_object()) {
            error = fallback + ": an action is an object {name, type, bindings}";
            return false;
        }
        if (!j.contains("name") || !j["name"].is_string()) {
            error = fallback + ": name must be a string";
            return false;
        }
        out = InputAction();
        out.name = trimmed(j["name"].get<std::string>());
        const std::string where = "action \"" + out.name + "\"";
        if (!checkKeys(j, {"name", "type", "description", "press_point", "bindings"}, where, error)) return false;
        if (j.contains("type")) {
            const std::string type = j["type"].is_string() ? lower(j["type"].get<std::string>()) : "";
            if (type == "axis") out.type = InputActionType::Axis;
            else if (type == "button") out.type = InputActionType::Button;
            else {
                error = where + ": type must be axis or button";
                return false;
            }
        }
        if (j.contains("description")) {
            if (!j["description"].is_string()) {
                error = where + ": description must be a string";
                return false;
            }
            out.description = j["description"].get<std::string>();
        }
        if (!readNumber(j, "press_point", out.pressPoint, where, error)) return false;
        if (j.contains("bindings")) {
            if (!j["bindings"].is_array()) {
                error = where + ": bindings must be a list";
                return false;
            }
            for (size_t b = 0; b < j["bindings"].size(); b++) {
                InputBinding binding;
                if (!bindingFromJson(j["bindings"][b], binding, where + " binding " + std::to_string(b + 1), error)) return false;
                out.bindings.push_back(binding);
            }
        }
        return true;
    }

    bool parseJsonText(const std::string& text, Json& out, std::string& error) {
        try {
            out = Json::parse(text);
            return true;
        } catch (const std::exception& e) {
            error = std::string("not valid JSON: ") + e.what();
            return false;
        }
    }

    bool validNumber(float value) {
        return std::isfinite(value) && value >= 0.0f;
    }

    void resetRuntime(ActionRuntime& runtime) {
        runtime.smoothed.clear();
        runtime.boundedValue = runtime.boundedRaw = runtime.deltaValue = 0.0f;
        runtime.pressed = runtime.down = runtime.up = false;
    }

    // the bounded and delta sums of the action's virtual sources
    void sumSources(const Registry& s, int index, float& bounded, float& delta) {
        bounded = 0.0f;
        delta = 0.0f;
        for (const Source& source : s.sources) {
            if (source.index != index) continue;
            if (source.delta) delta += source.value;
            else bounded += source.value;
        }
    }

    float clampFor(const InputAction& action, float value) {
        return std::clamp(value, action.type == InputActionType::Button ? 0.0f : -1.0f, 1.0f);
    }

}

// ---- definition

InputBinding InputActions::makeBinding(InputBindingKind kind) {
    InputBinding binding;
    binding.kind = kind;
    if (kind == InputBindingKind::GamepadAxis) {
        binding.gravity = 0.0f;
        binding.sensitivity = 1.0f;
        binding.dead = 0.19f;
    } else if (kind == InputBindingKind::MouseDelta) {
        binding.gravity = 0.0f;
        binding.sensitivity = 0.1f;
        binding.dead = 0.0f;
    }
    return binding;
}

std::vector<InputAction> InputActions::defaults() {
    auto key = [](int code) { return InputControl{InputDevice::Key, code}; };
    auto mouse = [](int code) { return InputControl{InputDevice::Mouse, code}; };
    auto pad = [](int code) { return InputControl{InputDevice::GamepadButton, code}; };

    auto keys = [](std::vector<InputControl> negative, std::vector<InputControl> positive) {
        InputBinding binding = makeBinding(InputBindingKind::Buttons);
        binding.negative = std::move(negative);
        binding.positive = std::move(positive);
        binding.snap = true;
        return binding;
    };
    auto stick = [](int axis, bool invert) {
        InputBinding binding = makeBinding(InputBindingKind::GamepadAxis);
        binding.axis = axis;
        binding.invert = invert;
        return binding;
    };
    // Unity's buttons: gravity and sensitivity 1000 (instant)
    auto button = [](std::vector<InputControl> positive) {
        InputBinding binding = makeBinding(InputBindingKind::Buttons);
        binding.positive = std::move(positive);
        binding.gravity = 1000.0f;
        binding.sensitivity = 1000.0f;
        return binding;
    };
    auto mouseMove = [](MouseDeltaAxis axis) {
        InputBinding binding = makeBinding(InputBindingKind::MouseDelta);
        binding.mouseAxis = axis;
        return binding;
    };
    auto action = [](const char* name, InputActionType type, const char* description, std::vector<InputBinding> bindings) {
        InputAction a;
        a.name = name;
        a.type = type;
        a.description = description;
        a.bindings = std::move(bindings);
        return a;
    };

    return {
        action("Horizontal", InputActionType::Axis, "Move left / right",
               {keys({key(D_KEY_LEFT), key(D_KEY_A)}, {key(D_KEY_RIGHT), key(D_KEY_D)}), stick(D_GAMEPAD_AXIS_LEFT_X, false)}),
        action("Vertical", InputActionType::Axis, "Move down / up",
               {keys({key(D_KEY_DOWN), key(D_KEY_S)}, {key(D_KEY_UP), key(D_KEY_W)}), stick(D_GAMEPAD_AXIS_LEFT_Y, true)}),
        action("Fire1", InputActionType::Button, "", {button({key(D_KEY_LEFT_CONTROL), mouse(D_MOUSE_BUTTON_LEFT)}), button({pad(D_GAMEPAD_BUTTON_A)})}),
        action("Fire2", InputActionType::Button, "", {button({key(D_KEY_LEFT_ALT), mouse(D_MOUSE_BUTTON_RIGHT)}), button({pad(D_GAMEPAD_BUTTON_B)})}),
        action("Fire3", InputActionType::Button, "", {button({key(D_KEY_LEFT_SHIFT), mouse(D_MOUSE_BUTTON_MIDDLE)}), button({pad(D_GAMEPAD_BUTTON_X)})}),
        action("Jump", InputActionType::Button, "", {button({key(D_KEY_SPACE)}), button({pad(D_GAMEPAD_BUTTON_Y)})}),
        action("Mouse X", InputActionType::Axis, "Mouse movement, right positive", {mouseMove(MouseDeltaAxis::X)}),
        action("Mouse Y", InputActionType::Axis, "Mouse movement, up positive", {mouseMove(MouseDeltaAxis::Y)}),
        action("Mouse ScrollWheel", InputActionType::Axis, "Mouse wheel, up positive", {mouseMove(MouseDeltaAxis::Wheel)}),
        action("Submit", InputActionType::Button, "", {button({key(D_KEY_ENTER), key(D_KEY_KP_ENTER), key(D_KEY_SPACE)}), button({pad(D_GAMEPAD_BUTTON_A)})}),
        action("Cancel", InputActionType::Button, "", {button({key(D_KEY_ESCAPE)}), button({pad(D_GAMEPAD_BUTTON_B)})}),
    };
}

void InputActions::loadDefaults() {
    setActions(defaults());
}

void InputActions::setActions(const std::vector<InputAction>& actions) {
    Registry& s = loaded();
    // the runtime of the names that remain stays (overrides, enabled, presses)
    std::map<std::string, ActionRuntime> previous;
    for (size_t i = 0; i < s.actions.size(); i++) previous[s.actions[i].name] = s.runtime[i];

    s.actions = actions;
    s.runtime.assign(actions.size(), ActionRuntime());
    for (size_t i = 0; i < actions.size(); i++) {
        auto it = previous.find(actions[i].name);
        if (it == previous.end()) continue;
        ActionRuntime& runtime = s.runtime[i];
        runtime.enabled = it->second.enabled;
        runtime.hasOverride = it->second.hasOverride;
        runtime.override = it->second.override;
        runtime.pressed = it->second.pressed;
        // the bindings may have changed: smoothing starts over
    }
    s.reportedUnknown.clear();
    rebuildIndex(s);
}

const std::vector<InputAction>& InputActions::getActions() {
    return loaded().actions;
}

bool InputActions::loadJson(const std::string& json, std::string* error) {
    std::vector<InputAction> actions;
    std::string why;
    if (!parseActionsJson(json, actions, &why)) {
        Log::error("Input actions not loaded: %s", why.c_str());
        if (error) *error = why;
        return false;
    }
    setActions(actions);
    return true;
}

std::string InputActions::toJson() {
    return actionsToJson(getActions());
}

bool InputActions::parseActionsJson(const std::string& json, std::vector<InputAction>& out, std::string* error) {
    std::string why;
    Json j;
    if (!parseJsonText(json, j, why)) {
        if (error) *error = why;
        return false;
    }
    const Json* list = &j;
    if (j.is_object()) {
        if (j.contains("version") && (!j["version"].is_number_integer() || j["version"].get<int>() != 1)) {
            if (error) *error = "input version " + j["version"].dump() + " is not supported (this engine reads version 1)";
            return false;
        }
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.key() != "version" && it.key() != "actions") {
                if (error) *error = "unknown field \"" + it.key() + "\" (fields: version, actions)";
                return false;
            }
        }
        if (!j.contains("actions")) {
            if (error) *error = "the input definition has no actions list";
            return false;
        }
        list = &j["actions"];
    }
    if (!list->is_array()) {
        if (error) *error = "actions must be a list";
        return false;
    }
    std::vector<InputAction> actions;
    for (size_t i = 0; i < list->size(); i++) {
        InputAction action;
        if (!actionFromJson((*list)[i], action, i, why)) {
            if (error) *error = why;
            return false;
        }
        actions.push_back(action);
    }
    why = validate(actions);
    if (!why.empty()) {
        if (error) *error = why;
        return false;
    }
    out = std::move(actions);
    return true;
}

std::string InputActions::actionsToJson(const std::vector<InputAction>& actions, int indent) {
    OJson list = OJson::array();
    for (const InputAction& action : actions) list.push_back(actionToJson(action));
    OJson j = OJson::object();
    j["version"] = 1;
    j["actions"] = list;
    return j.dump(indent);
}

bool InputActions::parseBindingsJson(const std::string& json, std::vector<InputBinding>& out, std::string* error,
                                     const std::string& action) {
    std::string why;
    Json j;
    if (!parseJsonText(json, j, why)) {
        if (error) *error = why;
        return false;
    }
    if (!j.is_array()) {
        if (error) *error = "bindings must be a list";
        return false;
    }
    const std::string where = action.empty() ? std::string("binding") : "action \"" + action + "\" binding";
    std::vector<InputBinding> bindings;
    for (size_t b = 0; b < j.size(); b++) {
        InputBinding binding;
        if (!bindingFromJson(j[b], binding, where + " " + std::to_string(b + 1), why)) {
            if (error) *error = why;
            return false;
        }
        bindings.push_back(binding);
    }
    why = validateBindings(bindings, action);
    if (!why.empty()) {
        if (error) *error = why;
        return false;
    }
    out = std::move(bindings);
    return true;
}

std::string InputActions::bindingsToJson(const std::vector<InputBinding>& bindings) {
    OJson list = OJson::array();
    for (const InputBinding& binding : bindings) list.push_back(bindingToJson(binding));
    return list.dump();
}

std::string InputActions::validateBindings(const std::vector<InputBinding>& bindings, const std::string& action) {
    const std::string prefix = action.empty() ? std::string("binding ") : "action \"" + action + "\" binding ";
    for (size_t b = 0; b < bindings.size(); b++) {
        const InputBinding& binding = bindings[b];
        const std::string where = prefix + std::to_string(b + 1);
        if (!validNumber(binding.gravity) || !validNumber(binding.sensitivity) || !validNumber(binding.dead))
            return where + ": gravity, sensitivity and dead must be finite numbers >= 0";
        if (binding.dead >= 1.0f)
            return where + ": dead must be below 1";
        if (binding.gamepad < -1 || binding.gamepad > 15)
            return where + ": gamepad must be any (-1) or 0-15";
        if (binding.kind == InputBindingKind::Buttons) {
            for (const std::vector<InputControl>* list : {&binding.positive, &binding.negative}) {
                for (const InputControl& control : *list) {
                    if (control.device == InputDevice::GamepadAxis || InputControlNames::name(control).empty())
                        return where + ": a buttons binding lists keys, mouse buttons and gamepad buttons";
                }
            }
        } else if (binding.kind == InputBindingKind::GamepadAxis) {
            if (binding.axis < 0 || binding.axis > D_GAMEPAD_AXIS_LAST)
                return where + ": unknown gamepad axis";
        }
    }
    return "";
}

std::string InputActions::validate(const std::vector<InputAction>& actions) {
    std::set<std::string> names;
    for (size_t i = 0; i < actions.size(); i++) {
        const InputAction& action = actions[i];
        if (trimmed(action.name).empty())
            return "action " + std::to_string(i + 1) + " has no name";
        if (trimmed(action.name) != action.name)
            return "action \"" + action.name + "\": the name has leading or trailing spaces";
        if (!names.insert(action.name).second)
            return "two actions are named \"" + action.name + "\" (names are unique, case-sensitive)";
        if (!std::isfinite(action.pressPoint) || action.pressPoint <= 0.0f || action.pressPoint > 1.0f)
            return "action \"" + action.name + "\": press_point must be above 0 and at most 1";
        const std::string problem = validateBindings(action.bindings, action.name);
        if (!problem.empty()) return problem;
    }
    return "";
}

const char* InputActions::typeName(InputActionType type) {
    return type == InputActionType::Button ? "button" : "axis";
}

const char* InputActions::kindName(InputBindingKind kind) {
    switch (kind) {
        case InputBindingKind::GamepadAxis: return "gamepad_axis";
        case InputBindingKind::MouseDelta: return "mouse_delta";
        default: return "buttons";
    }
}

const char* InputActions::mouseAxisName(MouseDeltaAxis axis) {
    switch (axis) {
        case MouseDeltaAxis::Y: return "y";
        case MouseDeltaAxis::Wheel: return "wheel";
        default: return "x";
    }
}

// ---- per frame

void InputActions::update(double unscaledDeltaTime, const InputSuppression& suppression) {
    Registry& s = loaded();
    s.frameDowns.clear();
    s.frameUps.clear();
    const float dt = static_cast<float>(std::max(0.0, unscaledDeltaTime));
    const Vector2 mouseDelta = Input::getMouseDelta();
    const Vector2 scrollDelta = Input::getMouseScrollDelta();

    for (size_t i = 0; i < s.actions.size(); i++) {
        const InputAction& action = s.actions[i];
        ActionRuntime& runtime = s.runtime[i];
        if (!runtime.enabled) {
            resetRuntime(runtime);
            continue;
        }
        const std::vector<InputBinding>& bindings = bindingsInUse(s, static_cast<int>(i));
        if (runtime.smoothed.size() != bindings.size()) runtime.smoothed.assign(bindings.size(), 0.0f);

        const bool isButton = action.type == InputActionType::Button;
        float bounded = 0.0f, raw = 0.0f, delta = 0.0f;
        bool pressed = false;
        bool tapped = false;   // a control pressed in this frame (released again within it too)

        for (size_t b = 0; b < bindings.size(); b++) {
            const InputBinding& binding = bindings[b];
            switch (binding.kind) {
                case InputBindingKind::Buttons: {
                    bool positive = false, negative = false;
                    auto scan = [&](const std::vector<InputControl>& controls, bool& any) {
                        for (const InputControl& control : controls) {
                            if (suppressed(control, suppression)) continue;
                            bool held, pressedThisFrame;
                            readControl(control, binding.gamepad, held, pressedThisFrame);
                            if (held) any = true;
                            if (pressedThisFrame) tapped = true;
                        }
                    };
                    scan(binding.positive, positive);
                    if (!isButton) scan(binding.negative, negative);

                    float target = (positive ? 1.0f : 0.0f) - (negative ? 1.0f : 0.0f);
                    if (binding.invert) target = -target;
                    float& smoothed = runtime.smoothed[b];
                    if (binding.snap && target != 0.0f && smoothed != 0.0f && sign(target) != sign(smoothed))
                        smoothed = 0.0f;
                    if (target != 0.0f)
                        smoothed = binding.sensitivity <= 0.0f ? target : moveTowards(smoothed, target, binding.sensitivity * dt);
                    else
                        smoothed = binding.gravity <= 0.0f ? 0.0f : moveTowards(smoothed, 0.0f, binding.gravity * dt);

                    bounded += std::fabs(smoothed) < binding.dead ? 0.0f : smoothed;
                    raw += target;
                    // the positive button (and on an axis, the negative one too) holds the button
                    if (positive || negative) pressed = true;
                    break;
                }
                case InputBindingKind::GamepadAxis: {
                    if (suppression.gamepadNavigation &&
                            (binding.axis == D_GAMEPAD_AXIS_LEFT_X || binding.axis == D_GAMEPAD_AXIS_LEFT_Y))
                        break;
                    float value = readGamepadAxis(binding.gamepad, binding.axis);
                    // dead zone rescaled: the output starts at 0 past it
                    const float magnitude = std::fabs(value);
                    value = magnitude <= binding.dead ? 0.0f : sign(value) * (magnitude - binding.dead) / (1.0f - binding.dead);
                    value *= binding.sensitivity;
                    if (binding.invert) value = -value;
                    bounded += value;
                    raw += value;
                    if ((isButton ? value : std::fabs(value)) >= action.pressPoint) pressed = true;
                    break;
                }
                case InputBindingKind::MouseDelta: {
                    float value = 0.0f;
                    switch (binding.mouseAxis) {
                        case MouseDeltaAxis::X: value = mouseDelta.x; break;
                        case MouseDeltaAxis::Y: value = -mouseDelta.y; break;     // canvas y grows down
                        case MouseDeltaAxis::Wheel: value = scrollDelta.y; break;
                    }
                    value *= binding.sensitivity;
                    if (binding.invert) value = -value;
                    delta += value;
                    break;
                }
            }
        }

        // an on-screen button writes 1: a bounded source past the press point holds the button
        for (const Source& source : s.sources) {
            if (source.index != static_cast<int>(i) || source.delta) continue;
            if ((isButton ? source.value : std::fabs(source.value)) >= action.pressPoint) pressed = true;
        }

        const bool wasPressed = runtime.pressed;
        runtime.boundedValue = bounded;
        runtime.boundedRaw = raw;
        runtime.deltaValue = delta;
        runtime.pressed = pressed;
        // a press and release inside one frame give the down and the up in that frame
        runtime.down = !wasPressed && (pressed || tapped);
        runtime.up = (wasPressed && !pressed) || (!wasPressed && !pressed && tapped);
        if (runtime.down) s.frameDowns.push_back(action.name);
        if (runtime.up) s.frameUps.push_back(action.name);
    }
}

void InputActions::endFrame() {
    Registry& s = loaded();
    s.sources.erase(std::remove_if(s.sources.begin(), s.sources.end(), [](const Source& source) { return source.delta; }),
                    s.sources.end());
}

const std::vector<std::string>& InputActions::getFrameButtonDowns() {
    return loaded().frameDowns;
}

const std::vector<std::string>& InputActions::getFrameButtonUps() {
    return loaded().frameUps;
}

// ---- queries

float InputActions::getAxis(const std::string& name) {
    const int index = find(name);
    if (index < 0) return 0.0f;
    Registry& s = loaded();
    const ActionRuntime& runtime = s.runtime[index];
    if (!runtime.enabled) return 0.0f;
    float bounded, delta;
    sumSources(s, index, bounded, delta);
    const InputAction& action = s.actions[index];
    const float value = clampFor(action, runtime.boundedValue + bounded) + runtime.deltaValue + delta;
    return action.type == InputActionType::Button ? std::clamp(value, 0.0f, 1.0f) : value;
}

float InputActions::getAxisRaw(const std::string& name) {
    const int index = find(name);
    if (index < 0) return 0.0f;
    Registry& s = loaded();
    const ActionRuntime& runtime = s.runtime[index];
    if (!runtime.enabled) return 0.0f;
    float bounded, delta;
    sumSources(s, index, bounded, delta);
    const InputAction& action = s.actions[index];
    const float value = clampFor(action, runtime.boundedRaw + bounded) + runtime.deltaValue + delta;
    return action.type == InputActionType::Button ? std::clamp(value, 0.0f, 1.0f) : value;
}

bool InputActions::getButton(const std::string& name) {
    const int index = find(name);
    return index >= 0 && loaded().runtime[index].enabled && loaded().runtime[index].pressed;
}

bool InputActions::getButtonDown(const std::string& name) {
    const int index = find(name);
    return index >= 0 && loaded().runtime[index].enabled && loaded().runtime[index].down;
}

bool InputActions::getButtonUp(const std::string& name) {
    const int index = find(name);
    return index >= 0 && loaded().runtime[index].enabled && loaded().runtime[index].up;
}

Vector2 InputActions::getVector(const std::string& xName, const std::string& yName) {
    Registry& s = loaded();
    auto boundedOf = [&s](const std::string& name) {
        const int index = find(name);
        if (index < 0 || !s.runtime[index].enabled) return 0.0f;
        float bounded, delta;
        sumSources(s, index, bounded, delta);
        return clampFor(s.actions[index], s.runtime[index].boundedValue + bounded);
    };
    Vector2 vector(boundedOf(xName), boundedOf(yName));
    const float length = vector.length();
    if (length > 1.0f) vector = vector / length;
    return vector;
}

bool InputActions::hasAction(const std::string& name) {
    return find(name, false) >= 0;
}

std::vector<std::string> InputActions::getActionNames() {
    std::vector<std::string> names;
    for (const InputAction& action : getActions()) names.push_back(action.name);
    return names;
}

void InputActions::setActionEnabled(const std::string& name, bool enabled) {
    const int index = find(name);
    if (index < 0) return;
    ActionRuntime& runtime = loaded().runtime[index];
    if (runtime.enabled == enabled) return;
    runtime.enabled = enabled;
    resetRuntime(runtime);
}

bool InputActions::isActionEnabled(const std::string& name) {
    const int index = find(name);
    return index >= 0 && loaded().runtime[index].enabled;
}

// ---- virtual sources

void InputActions::setSourceValue(uintptr_t owner, const std::string& action, float value, InputSourceKind kind) {
    Registry& s = loaded();
    if (!std::isfinite(value)) value = 0.0f;
    value = std::clamp(value, -1.0f, 1.0f);
    const int index = find(action);
    for (auto it = s.sources.begin(); it != s.sources.end(); ++it) {
        if (it->owner == owner && !it->delta && it->action == action) {
            if (value == 0.0f) s.sources.erase(it);
            else it->value = value;
            return;
        }
    }
    if (value == 0.0f || index < 0) return;
    Source source;
    source.owner = owner;
    source.action = action;
    source.index = index;
    source.value = value;
    source.kind = kind;
    s.sources.push_back(source);
}

void InputActions::addSourceDelta(uintptr_t owner, const std::string& action, float delta, InputSourceKind kind) {
    Registry& s = loaded();
    if (!std::isfinite(delta) || delta == 0.0f) return;
    const int index = find(action);
    if (index < 0) return;
    for (Source& source : s.sources) {
        if (source.owner == owner && source.delta && source.action == action) {
            source.value += delta;
            return;
        }
    }
    Source source;
    source.owner = owner;
    source.action = action;
    source.index = index;
    source.value = delta;
    source.delta = true;
    source.kind = kind;
    s.sources.push_back(source);
}

void InputActions::clearSource(uintptr_t owner) {
    Registry& s = loaded();
    s.sources.erase(std::remove_if(s.sources.begin(), s.sources.end(), [owner](const Source& source) { return source.owner == owner; }),
                    s.sources.end());
}

void InputActions::clearSources(InputSourceKind kind) {
    Registry& s = loaded();
    s.sources.erase(std::remove_if(s.sources.begin(), s.sources.end(), [kind](const Source& source) { return source.kind == kind; }),
                    s.sources.end());
}

void InputActions::clearAllSources() {
    loaded().sources.clear();
}

// the top bit: no user-space address has it
static constexpr uintptr_t namedSourceBit = static_cast<uintptr_t>(1) << (sizeof(uintptr_t) * 8 - 1);

uintptr_t InputActions::namedSource(const std::string& name) {
    Registry& s = loaded();
    auto it = std::find(s.namedSources.begin(), s.namedSources.end(), name);
    size_t index = static_cast<size_t>(it - s.namedSources.begin());
    if (it == s.namedSources.end()) s.namedSources.push_back(name);
    return namedSourceBit | static_cast<uintptr_t>(index + 1);
}

bool InputActions::isNamedSource(uintptr_t owner) {
    return (owner & namedSourceBit) != 0;
}

// ---- rebinding

bool InputActions::setBindings(const std::string& action, const std::vector<InputBinding>& bindings, std::string* error) {
    const int index = find(action);
    if (index < 0) {
        if (error) *error = "Input action \"" + action + "\" does not exist (Project Settings > Input)";
        return false;
    }
    const std::string problem = validateBindings(bindings, action);
    if (!problem.empty()) {
        if (error) *error = problem;
        return false;
    }
    ActionRuntime& runtime = loaded().runtime[index];
    runtime.hasOverride = true;
    runtime.override = bindings;
    runtime.smoothed.clear();
    return true;
}

std::vector<InputBinding> InputActions::getBindings(const std::string& action) {
    const int index = find(action);
    if (index < 0) return {};
    return bindingsInUse(loaded(), index);
}

bool InputActions::hasBindingOverride(const std::string& action) {
    const int index = find(action, false);
    return index >= 0 && loaded().runtime[index].hasOverride;
}

void InputActions::resetBindings(const std::string& action) {
    Registry& s = loaded();
    for (size_t i = 0; i < s.actions.size(); i++) {
        if (!action.empty() && s.actions[i].name != action) continue;
        if (!s.runtime[i].hasOverride) continue;
        s.runtime[i].hasOverride = false;
        s.runtime[i].override.clear();
        s.runtime[i].smoothed.clear();
    }
    if (!action.empty()) find(action);
}

std::string InputActions::exportOverrides() {
    Registry& s = loaded();
    OJson bindings = OJson::object();
    for (size_t i = 0; i < s.actions.size(); i++) {
        if (!s.runtime[i].hasOverride) continue;
        OJson list = OJson::array();
        for (const InputBinding& binding : s.runtime[i].override) list.push_back(bindingToJson(binding));
        bindings[s.actions[i].name] = list;
    }
    OJson j = OJson::object();
    j["version"] = 1;
    j["bindings"] = bindings;
    return j.dump();
}

bool InputActions::importOverrides(const std::string& json, std::string* error) {
    std::string why;
    Json j;
    if (!parseJsonText(json, j, why)) {
        if (error) *error = why;
        return false;
    }
    if (!j.is_object() || !j.contains("bindings") || !j["bindings"].is_object()) {
        if (error) *error = "expected {\"version\": 1, \"bindings\": {\"Action\": [...]}} (what exportBindings returns)";
        return false;
    }
    if (j.contains("version") && (!j["version"].is_number_integer() || j["version"].get<int>() != 1)) {
        if (error) *error = "bindings version " + j["version"].dump() + " is not supported";
        return false;
    }
    Registry& s = loaded();
    std::map<int, std::vector<InputBinding>> overrides;
    for (auto it = j["bindings"].begin(); it != j["bindings"].end(); ++it) {
        const int index = find(it.key(), false);
        if (index < 0) {
            Log::warn("Input bindings of \"%s\" skipped: the game has no such action", it.key().c_str());
            continue;
        }
        std::vector<InputBinding> bindings;
        if (!parseBindingsJson(it.value().dump(), bindings, &why, it.key())) {
            if (error) *error = why;
            return false;
        }
        overrides[index] = bindings;
    }
    for (size_t i = 0; i < s.actions.size(); i++) {
        ActionRuntime& runtime = s.runtime[i];
        auto it = overrides.find(static_cast<int>(i));
        const bool had = runtime.hasOverride;
        runtime.hasOverride = it != overrides.end();
        runtime.override = runtime.hasOverride ? it->second : std::vector<InputBinding>();
        if (had || runtime.hasOverride) runtime.smoothed.clear();
    }
    return true;
}

void InputActions::resetRuntimeState() {
    Registry& s = loaded();
    for (ActionRuntime& runtime : s.runtime) runtime = ActionRuntime();
    s.sources.clear();
    s.frameDowns.clear();
    s.frameUps.clear();
    s.reportedUnknown.clear();
}

bool InputActions::getState(const std::string& name, State& out) {
    const int index = find(name, false);
    if (index < 0) return false;
    out.value = getAxis(name);
    out.raw = getAxisRaw(name);
    out.pressed = getButton(name);
    out.enabled = isActionEnabled(name);
    return true;
}
