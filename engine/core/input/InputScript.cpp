// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Only with the input scripts module (input/InputScriptSwitch.h); InputScriptsOff.cpp answers the
// engine without it
#include "input/InputScriptSwitch.h"
#if JASPERENGINE_INPUT_SCRIPTS_ENABLED

#include "input/InputScript.h"
#include "input/InputScriptRunner.h"
#include "input/InputRecorder.h"
#include "Log.h"

#include "Engine.h"
#include "Input.h"
#include "Screen.h"
#include "input/InputActions.h"
#include "input/InputControlNames.h"
#include "math/Rect.h"
#include "script/LuaBinding.h"
#include "subsystem/GamepadCursor.h"
#include "util/PlatformRules.h"

#include "lua.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace jasperengine::input {

namespace {

    std::string lowerText(std::string text){
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    // a value of an event: its number, else the default (send_input's own clamps)
    int intOf(const ScriptJson& event, const char* key, int fallback, int lo, int hi){
        auto it = event.find(key);
        if (it == event.end() || !it->is_number())
            return fallback;
        const double v = it->get<double>();
        if (!(v == v))
            return fallback;
        return static_cast<int>(std::clamp(v, static_cast<double>(lo), static_cast<double>(hi)));
    }

    double doubleOf(const ScriptJson& event, const char* key, double fallback){
        auto it = event.find(key);
        return it != event.end() && it->is_number() ? it->get<double>() : fallback;
    }

    std::string stringOf(const ScriptJson& event, const char* key, const std::string& fallback = std::string()){
        auto it = event.find(key);
        return it != event.end() && it->is_string() ? it->get<std::string>() : fallback;
    }

    int parseKeyValue(const ScriptJson& value){
        if (value.is_number_integer())
            return value.get<int>();
        if (!value.is_string())
            return D_KEY_UNKNOWN;
        return InputControlNames::parseKey(value.get<std::string>());
    }

    int parseMouseButtonValue(const ScriptJson& value){
        if (value.is_null())
            return D_MOUSE_BUTTON_LEFT;
        if (value.is_number_integer())
            return std::clamp(value.get<int>(), 0, D_MOUSE_BUTTON_LAST);
        const std::string name = value.is_string() ? value.get<std::string>() : std::string();
        if (name.empty())
            return D_MOUSE_BUTTON_LEFT;
        return InputControlNames::parseMouseButton(name);
    }

    int parseGamepadButtonValue(const ScriptJson& value){
        if (value.is_number_integer())
            return std::clamp(value.get<int>(), 0, D_GAMEPAD_BUTTON_LAST);
        if (!value.is_string())
            return -1;
        return InputControlNames::parseGamepadButton(value.get<std::string>());
    }

    int parseGamepadAxisValue(const ScriptJson& value){
        if (value.is_number_integer())
            return std::clamp(value.get<int>(), 0, D_GAMEPAD_AXIS_LAST);
        if (!value.is_string())
            return -1;
        return InputControlNames::parseGamepadAxis(value.get<std::string>());
    }

    int parseModsValue(const ScriptJson& value){
        int mods = 0;
        if (!value.is_array())
            return 0;
        for (const ScriptJson& item : value) {
            const std::string name = item.is_string() ? lowerText(item.get<std::string>()) : std::string();
            if (name == "shift") mods |= D_MODIFIER_SHIFT;
            else if (name == "ctrl" || name == "control") mods |= D_MODIFIER_CONTROL;
            else if (name == "alt") mods |= D_MODIFIER_ALT;
            else if (name == "super" || name == "cmd" || name == "meta") mods |= D_MODIFIER_SUPER;
        }
        return mods;
    }

    ScriptJson modsJson(int mods){
        ScriptJson out = ScriptJson::array();
        if (mods & D_MODIFIER_SHIFT) out.push_back("shift");
        if (mods & D_MODIFIER_CONTROL) out.push_back("ctrl");
        if (mods & D_MODIFIER_ALT) out.push_back("alt");
        if (mods & D_MODIFIER_SUPER) out.push_back("super");
        return out;
    }

    // UTF-8 -> code points
    std::vector<uint32_t> decodeUtf8(const std::string& text){
        std::vector<uint32_t> out;
        for (size_t i = 0; i < text.size();) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            uint32_t cp = c;
            size_t len = 1;
            if (c >= 0xF0) { cp = c & 0x07; len = 4; }
            else if (c >= 0xE0) { cp = c & 0x0F; len = 3; }
            else if (c >= 0xC0) { cp = c & 0x1F; len = 2; }
            for (size_t k = 1; k < len && i + k < text.size(); k++)
                cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
            out.push_back(cp);
            i += len;
        }
        return out;
    }

    bool readPoint(const ScriptJson& event, const char* xKey, const char* yKey, const char* arrayKey, double& x, double& y){
        auto arr = event.find(arrayKey);
        if (arr != event.end() && arr->is_array() && arr->size() >= 2 && (*arr)[0].is_number() && (*arr)[1].is_number()) {
            x = (*arr)[0].get<double>();
            y = (*arr)[1].get<double>();
            return true;
        }
        auto ix = event.find(xKey);
        auto iy = event.find(yKey);
        if (ix != event.end() && iy != event.end() && ix->is_number() && iy->is_number()) {
            x = ix->get<double>();
            y = iy->get<double>();
            return true;
        }
        return false;
    }

    // Collects one event's actions
    struct EventBuilder {
        ParsedScript& out;
        const EventParseOptions& options;
        uint32_t event;
        int gap;

        int32_t addString(const std::string& text){
            out.strings.push_back(text);
            return static_cast<int32_t>(out.strings.size() - 1);
        }

        ScriptAction& add(ActionType type, int framesAfter){
            ScriptAction action;
            action.type = type;
            action.event = event;
            action.framesAfter = static_cast<uint32_t>(std::max(framesAfter, 0));
            out.actions.push_back(action);
            return out.actions.back();
        }

        // an action held for hold_frames / hold_ms: a file's ms are frames at its fixed_fps (R3),
        // as a live call's are under play fixed_fps
        ScriptAction& addHeld(ActionType type, int holdFrames, int holdMs){
            if (holdMs > 0 && !options.live) {
                const int fps = std::max(options.fixedFps, 1);
                const int msFrames = static_cast<int>(std::ceil(holdMs * static_cast<double>(fps) / 1000.0 - 1e-9));
                return add(type, std::max({holdFrames, msFrames, 1}));
            }
            ScriptAction& action = add(type, holdFrames);
            action.msAfter = static_cast<uint32_t>(std::max(holdMs, 0));
            return action;
        }

        // {xKey, yKey} / arrayKey [x,y] / targetKey; false + error when the target is not a selector
        bool readEventPoint(const ScriptJson& eventJson, const char* xKey, const char* yKey, const char* arrayKey,
                            const char* targetKey, Space space, ScriptPoint& point, std::string& error){
            point.space = space;
            auto t = eventJson.find(targetKey);
            if (t != eventJson.end() && !t->is_null()) {
                if (!t->is_string() || t->get<std::string>().empty()) {
                    error = std::string(targetKey) + " must be a CSS selector of a UI Document element";
                    return false;
                }
                ScriptTarget target;
                target.selector = t->get<std::string>();
                auto d = eventJson.find("document");
                target.document = d != eventJson.end() ? *d : ScriptJson();
                out.targets.push_back(std::move(target));
                point.target = static_cast<int32_t>(out.targets.size() - 1);
                point.given = true;
                return true;
            }
            point.given = readPoint(eventJson, xKey, yKey, arrayKey, point.x, point.y);
            return true;
        }
    };

    std::string eventLabel(const std::string& type, const ScriptJson& event){
        std::string action = stringOf(event, "action");
        if (type == "action")
            action = stringOf(event, "phase", "press");
        else if (type == "key" || type == "input_action" || type == "touch" || type == "mouse")
            action = action.empty() ? (type == "touch" ? "tap" : type == "mouse" ? "click" : "press") : action;
        else if (type == "gamepad" && action.empty())
            action = event.contains("axis") ? "axis" : "press";
        else if (type == "wait")
            action = event.contains("until") ? "until" : std::string();
        else if (type != "app")
            action.clear();
        return action.empty() ? type : type + " " + lowerText(action);
    }

    // One event -> its actions; false + error for a malformed event (send_input's messages)
    bool buildEvent(const ScriptJson& event, uint32_t index, const EventParseOptions& options, ParsedScript& out, std::string& error){
        if (!event.is_object()) {
            error = "each event must be an object with a type";
            return false;
        }
        const std::string type = lowerText(stringOf(event, "type"));
        Space space = options.space;
        if (event.contains("space")) {
            const std::string spaceText = lowerText(stringOf(event, "space"));
            if (!parseSpace(spaceText, space)) {
                error = "space must be canvas, normalized, safe_area or view";
                return false;
            }
        }
        if (space == Space::View && !options.live) {
            error = "space \"view\" is the editor's window and cannot be stored: use normalized, canvas or safe_area";
            return false;
        }
        EventBuilder b{out, options, index, options.gapFrames};
        const int gap = options.gapFrames;
        // 0: the press and its release in the same frame (a tap Input.getKeyDown and getKeyUp both see)
        const int holdFrames = intOf(event, "hold_frames", 2, 0, 100000);
        const int holdMs = intOf(event, "hold_ms", 0, 0, 600000);
        const int mods = parseModsValue(event.contains("mods") ? event["mods"] : ScriptJson::array());

        if (type == "wait") {
            const int frames = intOf(event, "frames", 0, 0, 1000000);
            const int ms = intOf(event, "ms", 0, 0, 600000);
            if (event.contains("until")) {
                const std::string until = stringOf(event, "until");
                if (until.empty()) {
                    error = "wait until must be a Lua condition, e.g. \"Player ~= nil and Player.grounded\"";
                    return false;
                }
                ScriptAction& a = b.add(ActionType::WaitUntil, 0);
                a.string = b.addString(until);
                a.count = event.contains("frames") ? std::max(frames, 1) : WAIT_UNTIL_DEFAULT_FRAMES;
                return true;
            }
            if (frames == 0 && ms == 0) {
                error = "wait needs frames, ms or until";
                return false;
            }
            if (ms > 0 && !options.live) {
                const int fps = std::max(options.fixedFps, 1);
                const int msFrames = static_cast<int>(std::ceil(ms * static_cast<double>(fps) / 1000.0 - 1e-9));
                b.add(ActionType::Wait, std::max({frames, msFrames, 1}));
            } else {
                b.add(ActionType::Wait, frames).msAfter = static_cast<uint32_t>(ms);
            }
            return true;
        }

        if (type == "shot") {
            const std::string name = stringOf(event, "name");
            if (name.empty() || name.find_first_of("/\\:") != std::string::npos || name == "." || name == "..") {
                error = "shot needs name, a file name without a folder (e.g. \"01_menu\")";
                return false;
            }
            b.add(ActionType::Shot, gap).string = b.addString(name);
            return true;
        }

        if (type == "key") {
            const ScriptJson keyValue = event.contains("key") ? event["key"] : ScriptJson();
            const int key = parseKeyValue(keyValue);
            if (key == D_KEY_UNKNOWN) {
                error = "unknown key " + keyValue.dump() + " (names like space, enter, escape, a-z, 0-9, left, right, up, down, "
                        "shift, ctrl, alt, tab, backspace, f1-f12, kp_0-kp_9)";
                return false;
            }
            const std::string action = lowerText(stringOf(event, "action", "press"));
            if (action == "down" || action == "up") {
                ScriptAction& a = b.add(action == "down" ? ActionType::KeyDown : ActionType::KeyUp, gap);
                a.code = key;
                a.mods = mods;
            } else if (action == "press" || action == "tap") {
                ScriptAction& down = b.addHeld(ActionType::KeyDown, holdFrames, holdMs);
                down.code = key;
                down.mods = mods;
                ScriptAction& up = b.add(ActionType::KeyUp, gap);
                up.code = key;
                up.mods = mods;
            } else {
                error = "key action must be press, down or up";
                return false;
            }
            return true;
        }

        if (type == "text") {
            const std::string text = stringOf(event, "text");
            if (text.empty()) {
                error = "text event needs text";
                return false;
            }
            if (event.contains("per_unit") && !event["per_unit"].is_boolean()) {
                error = "per_unit must be true or false";
                return false;
            }
            // per_unit: one character event per UTF-16 unit, as Windows WM_CHAR and Android's keys
            // deliver typing; else the whole string at once (a paste, an IME commit)
            const bool perUnit = event.value("per_unit", false);
            b.add(perUnit ? ActionType::TextUnits : ActionType::Text, gap).string = b.addString(text);
            return true;
        }

        // IME composition of the focused HTML UI text field (cursor in code points, -1 selects it)
        if (type == "ime") {
            const std::string text = stringOf(event, "text");
            const int cursor = event.contains("cursor") && event["cursor"].is_number_integer()
                ? event["cursor"].get<int>() : static_cast<int>(decodeUtf8(text).size());
            ScriptAction& a = b.add(ActionType::Ime, gap);
            a.string = b.addString(text);
            a.count = cursor;
            return true;
        }
        if (type == "ime_commit") {
            b.add(ActionType::ImeCommit, gap).string = b.addString(stringOf(event, "text"));
            return true;
        }
        if (type == "ime_cancel") {
            b.add(ActionType::ImeCancel, gap);
            return true;
        }

        if (type == "mouse_leave" || type == "mouse_enter") {
            b.add(type == "mouse_leave" ? ActionType::MouseLeave : ActionType::MouseEnter, gap);
            return true;
        }

        if (type == "mouse") {
            const std::string action = lowerText(stringOf(event, "action", "click"));
            const int button = parseMouseButtonValue(event.contains("button") ? event["button"] : ScriptJson());
            if (button < 0) {
                error = "button must be left, right, middle or button4..button8";
                return false;
            }
            ScriptPoint from, to;
            if (!b.readEventPoint(event, "x", "y", "position", "target", space, from, error) ||
                !b.readEventPoint(event, "to_x", "to_y", "to", "to_target", space, to, error))
                return false;
            // a move without a point and with dx, dy goes that far from the pointer (mouse-look)
            const bool relativeMove = action == "move" && !from.given && (event.contains("dx") || event.contains("dy"));
            if (!from.given && !relativeMove && (action == "move" || action == "drag")) {
                error = "mouse " + action + " needs x and y (or position [x,y], or target), or dx/dy for a move";
                return false;
            }
            if (to.target >= 0 && action != "drag") {
                error = "to_target is for mouse drag and touch swipe";
                return false;
            }
            auto mouse = [&](ActionType t, int framesAfter, const ScriptPoint& point) -> ScriptAction& {
                ScriptAction& a = b.add(t, framesAfter);
                a.code = button;
                a.mods = mods;
                a.point = point;
                return a;
            };
            if (relativeMove) {
                ScriptPoint delta;
                delta.space = space;
                ScriptAction& a = mouse(ActionType::MouseMoveBy, gap, delta);
                a.value = static_cast<float>(doubleOf(event, "dx", 0.0));
                a.value2 = static_cast<float>(doubleOf(event, "dy", 0.0));
            } else if (action == "move") {
                mouse(ActionType::MouseMove, gap, from);
            } else if (action == "down" || action == "up") {
                mouse(action == "down" ? ActionType::MouseDown : ActionType::MouseUp, gap, from);
            } else if (action == "click" || action == "double_click") {
                // the up, and a double click's second press, happen where the first press was
                const int clicks = action == "double_click" ? 2 : 1;
                for (int i = 0; i < clicks; i++) {
                    ScriptAction& down = b.addHeld(ActionType::MouseDown, holdFrames, holdMs);
                    down.code = button;
                    down.mods = mods;
                    if (i == 0)
                        down.point = from;
                    mouse(ActionType::MouseUp, gap, ScriptPoint());
                }
            } else if (action == "drag") {
                if (!to.given) {
                    error = "mouse drag needs to: [x,y] (or to_x, to_y, or to_target)";
                    return false;
                }
                const int steps = intOf(event, "steps", 10, 1, 1000);
                ScriptAction& start = mouse(ActionType::DragStart, 1, from);
                start.to = to;
                for (int i = 1; i <= steps; i++)
                    mouse(ActionType::DragMove, 1, ScriptPoint()).value = float(i) / steps;
                mouse(ActionType::DragEnd, gap, ScriptPoint());
            } else if (action == "scroll") {
                ScriptAction& a = mouse(ActionType::MouseScroll, gap, from);
                a.value2 = static_cast<float>(doubleOf(event, "dx", 0.0));
                a.value = static_cast<float>(doubleOf(event, "dy", doubleOf(event, "amount", 1.0)));
            } else {
                error = "mouse action must be click, double_click, down, up, move, drag or scroll";
                return false;
            }
            return true;
        }

        if (type == "touch") {
            const std::string action = lowerText(stringOf(event, "action", "tap"));
            const int pointer = intOf(event, "pointer", 0, 0, 9);
            ScriptPoint from, to;
            if (!b.readEventPoint(event, "x", "y", "position", "target", space, from, error) ||
                !b.readEventPoint(event, "to_x", "to_y", "to", "to_target", space, to, error))
                return false;
            if (!from.given && action != "end" && action != "cancel") {
                error = "touch " + action + " needs x and y (or target)";
                return false;
            }
            if (to.target >= 0 && action != "swipe") {
                error = "to_target is for mouse drag and touch swipe";
                return false;
            }
            auto touch = [&](ActionType t, int framesAfter, const ScriptPoint& point) -> ScriptAction& {
                ScriptAction& a = b.add(t, framesAfter);
                a.device = pointer;
                a.point = point;
                return a;
            };
            if (action == "start" || action == "move") {
                touch(action == "start" ? ActionType::TouchStart : ActionType::TouchMove, gap, from);
            } else if (action == "end") {
                touch(ActionType::TouchEnd, gap, from);
            } else if (action == "cancel") {
                touch(ActionType::TouchCancel, gap, ScriptPoint());
            } else if (action == "tap") {
                // the end happens where the finger is
                ScriptAction& start = b.addHeld(ActionType::TouchStart, holdFrames, holdMs);
                start.device = pointer;
                start.point = from;
                touch(ActionType::TouchEnd, gap, ScriptPoint());
            } else if (action == "swipe") {
                if (!to.given) {
                    error = "touch swipe needs to: [x,y] (or to_target)";
                    return false;
                }
                const int steps = intOf(event, "steps", 10, 1, 1000);
                touch(ActionType::SwipeStart, 1, from).to = to;
                for (int i = 1; i <= steps; i++)
                    touch(ActionType::SwipeMove, 1, ScriptPoint()).value = float(i) / steps;
                touch(ActionType::SwipeEnd, gap, ScriptPoint());
            } else {
                error = "touch action must be tap, start, move, end, cancel or swipe";
                return false;
            }
            return true;
        }

        if (type == "gamepad") {
            const int pad = intOf(event, "gamepad", 0, 0, 15);
            const std::string action = lowerText(stringOf(event, "action", event.contains("axis") ? "axis" : "press"));
            // a call's or a script's button connects its gamepad when it is not; a recording says
            // so with its own connect event
            const int implicitConnect = options.recording ? 0 : 1;
            if (action == "connect") {
                ScriptAction& a = b.add(ActionType::GamepadConnect, gap);
                a.device = pad;
                a.string = b.addString(stringOf(event, "name", "MCP Virtual Gamepad"));
                return true;
            }
            if (action == "disconnect") {
                b.add(ActionType::GamepadDisconnect, gap).device = pad;
                return true;
            }
            // x, y or target: the gamepad cursor goes there first, in the same frame
            ScriptPoint warp;
            if (!b.readEventPoint(event, "x", "y", "position", "target", space, warp, error))
                return false;
            if (warp.given) {
                ScriptAction& a = b.add(ActionType::GamepadCursor, 0);
                a.device = pad;
                a.point = warp;
            }
            if (action == "axis") {
                const int axis = parseGamepadAxisValue(event.contains("axis") ? event["axis"] : ScriptJson());
                if (axis < 0) {
                    error = "axis must be left_x, left_y, right_x, right_y, left_trigger or right_trigger";
                    return false;
                }
                ScriptAction& a = b.add(ActionType::GamepadAxis, gap);
                a.device = pad;
                a.code = axis;
                a.count = implicitConnect;
                a.value = static_cast<float>(std::clamp(doubleOf(event, "value", 0.0), -1.0, 1.0));
                return true;
            }
            const int button = parseGamepadButtonValue(event.contains("button") ? event["button"] : ScriptJson());
            if (button < 0) {
                error = "gamepad button must be a, b, x, y, left_bumper, right_bumper, back, start, guide, left_thumb, "
                        "right_thumb, dpad_up, dpad_right, dpad_down, dpad_left";
                return false;
            }
            auto pressAction = [&](ActionType t, ScriptAction& a) {
                a.type = t;
                a.device = pad;
                a.code = button;
                a.count = implicitConnect;
            };
            if (action == "down") {
                pressAction(ActionType::GamepadButtonDown, b.add(ActionType::GamepadButtonDown, gap));
            } else if (action == "up") {
                pressAction(ActionType::GamepadButtonUp, b.add(ActionType::GamepadButtonUp, gap));
            } else if (action == "press") {
                pressAction(ActionType::GamepadButtonDown, b.addHeld(ActionType::GamepadButtonDown, holdFrames, holdMs));
                pressAction(ActionType::GamepadButtonUp, b.add(ActionType::GamepadButtonUp, gap));
            } else {
                error = "gamepad action must be press, down, up, axis, connect or disconnect";
                return false;
            }
            return true;
        }

        if (type == "action" || type == "axis" || type == "axis_delta") {
            // the Input Manager's actions (a flat name or Map/Action), through the run's virtual
            // source (as an on-screen control)
            const ScriptJson nameValue = event.contains("name") ? event["name"] : (event.contains("action") ? event["action"] : ScriptJson());
            const std::string name = nameValue.is_string() ? nameValue.get<std::string>() : std::string();
            // a live call checks now (the game runs); a file's actions are checked when they run
            if (options.live ? !InputActions::hasAction(name) : name.empty()) {
                std::string list;
                for (const std::string& known : InputActions::getActionNames())
                    list += (list.empty() ? "" : ", ") + known;
                error = (name.empty() ? std::string("name must be an input action") : "no input action \"" + name + "\"") +
                        " (Project Settings > Input: " + list + ")";
                return false;
            }
            const int32_t nameIndex = b.addString(name);
            if (type == "action") {
                const std::string phase = lowerText(stringOf(event, "phase", "press"));
                if (phase == "down" || phase == "up") {
                    b.add(phase == "down" ? ActionType::ActionDown : ActionType::ActionUp, gap).string = nameIndex;
                } else if (phase == "press" || phase == "tap") {
                    b.addHeld(ActionType::ActionDown, holdFrames, holdMs).string = nameIndex;
                    b.add(ActionType::ActionUp, gap).string = nameIndex;
                } else {
                    error = "action phase must be press, down or up";
                    return false;
                }
                return true;
            }
            const char* key = type == "axis" ? "value" : "delta";
            if (!event.contains(key) || !event[key].is_number()) {
                error = type + " needs a number " + key;
                return false;
            }
            if (type == "axis_delta") {
                ScriptAction& a = b.add(ActionType::AxisDelta, gap);
                a.string = nameIndex;
                a.value = event[key].get<float>();
                return true;
            }
            const float value = std::clamp(event[key].get<float>(), -1.0f, 1.0f);
            // with hold_frames / hold_ms it goes back to 0 after them, else it stays (0 releases)
            if (value != 0.0f && (event.contains("hold_frames") || event.contains("hold_ms"))) {
                ScriptAction& a = b.addHeld(ActionType::Axis, holdFrames, holdMs);
                a.string = nameIndex;
                a.value = value;
                b.add(ActionType::Axis, gap).string = nameIndex;
            } else {
                ScriptAction& a = b.add(ActionType::Axis, gap);
                a.string = nameIndex;
                a.value = value;
            }
            return true;
        }

        // an Input Action forced at top priority (InputActions::inject): press for hold_frames
        // windows (0: pressed and released in one), down until up, up, or a value for hold_frames
        if (type == "input_action") {
            const std::string name = stringOf(event, "name");
            if (name.find('/') == std::string::npos) {
                error = "input_action needs name \"Map/Action\"";
                return false;
            }
            const std::string action = lowerText(stringOf(event, "action", "press"));
            Vector2 value(1.0f, 0.0f);
            if (event.contains("value")) {
                const ScriptJson& v = event["value"];
                if (v.is_number()) {
                    value = Vector2(v.get<float>(), 0.0f);
                } else if (v.is_array() && v.size() == 2 && v[0].is_number() && v[1].is_number()) {
                    value = Vector2(v[0].get<float>(), v[1].get<float>());
                } else {
                    error = "input_action value is a number or [x, y]";
                    return false;
                }
            }
            const int32_t nameIndex = b.addString(name);
            if (action == "press" || action == "value") {
                ScriptAction& a = b.addHeld(ActionType::InputActionPress, holdFrames, holdMs);
                a.string = nameIndex;
                a.count = holdFrames;
                a.value = value.x;
                a.value2 = value.y;
                a.code = action == "value" ? 1 : 0;
                b.add(ActionType::Wait, gap);
            } else if (action == "down") {
                ScriptAction& a = b.add(ActionType::InputActionDown, gap);
                a.string = nameIndex;
                a.value = value.x;
                a.value2 = value.y;
            } else if (action == "up") {
                b.add(ActionType::InputActionUp, gap).string = nameIndex;
            } else {
                error = "input_action action must be press, down, up or value";
                return false;
            }
            return true;
        }

        if (type == "release_all") {
            b.add(ActionType::ReleaseAll, gap);
            return true;
        }

        // The app's lifecycle as a device reports it, or the simulated device turned
        if (type == "app") {
            const std::string action = lowerText(stringOf(event, "action"));
            static const std::pair<const char*, ActionType> appActions[] = {
                {"pause", ActionType::AppPause}, {"resume", ActionType::AppResume}, {"focus_lost", ActionType::AppFocusLost},
                {"focus_gained", ActionType::AppFocusGained}, {"low_memory", ActionType::AppLowMemory},
                {"hide", ActionType::AppHide}, {"show", ActionType::AppShow},
                {"view_lost", ActionType::AppViewLost}, {"view_restored", ActionType::AppViewRestored}};
            for (const auto& [name, actionType] : appActions) {
                if (action == name) {
                    b.add(actionType, gap);
                    return true;
                }
            }
            if (action == "orientation") {
                ScreenOrientation orientation;
                const std::string name = stringOf(event, "orientation");
                if (!PlatformRules::parseOrientationName(name, orientation) || !PlatformRules::isFixedOrientation(orientation)) {
                    error = "app orientation needs orientation: portrait, portrait_upside_down, landscape_left or landscape_right";
                    return false;
                }
                b.add(ActionType::AppOrientation, gap).string = b.addString(PlatformRules::orientationName(orientation));
                return true;
            }
            if (action == "back") {
                // an agent's Back, or a recorded Back button's (Engine.onBack hears which)
                const std::string source = lowerText(stringOf(event, "source", "agent"));
                if (source != "agent" && source != "back_button") {
                    error = "app back source must be agent or back_button (an Escape or a B is its key or button event)";
                    return false;
                }
                b.add(ActionType::AppBack, gap).code = static_cast<int>(source == "back_button" ? BackSource::BackButton : BackSource::Agent);
                return true;
            }
            error = "app action must be pause, resume, focus_lost, focus_gained, low_memory, hide, show, view_lost, view_restored, orientation or back";
            return false;
        }

        error = "type must be key, text, ime, ime_commit, ime_cancel, mouse, mouse_leave, mouse_enter, touch, gamepad, "
                "input_action, action, axis, axis_delta, app, wait, shot or release_all";
        return false;
    }

    std::atomic<int> injectingDepth{0};

    struct InjectScope {
        InjectScope() { injectingDepth.fetch_add(1, std::memory_order_relaxed); }
        ~InjectScope() { injectingDepth.fetch_sub(1, std::memory_order_relaxed); }
    };

    // a whole number >= 0, signed or not: a parsed file's are unsigned, a decoded Lua table's signed
    bool nonNegativeWhole(const ScriptJson& v, uint64_t& out){
        if (v.is_number_unsigned()) {
            out = v.get<uint64_t>();
            return true;
        }
        if (v.is_number_integer() && v.get<int64_t>() >= 0) {
            out = static_cast<uint64_t>(v.get<int64_t>());
            return true;
        }
        return false;
    }

    double scriptRound4(double v){
        return std::round(v * 10000.0) / 10000.0;
    }

    void clearScript(ParsedScript& out){
        out.actions.clear();
        out.strings.clear();
        out.targets.clear();
        out.eventLabels.clear();
        out.events = 0;
    }

}

// ================================================================== names

const char* spaceName(Space space){
    switch (space) {
        case Space::Canvas: return "canvas";
        case Space::Normalized: return "normalized";
        case Space::SafeArea: return "safe_area";
        case Space::View: return "view";
    }
    return "canvas";
}

bool parseSpace(std::string_view name, Space& out){
    if (name == "canvas") out = Space::Canvas;
    else if (name == "normalized") out = Space::Normalized;
    else if (name == "safe_area") out = Space::SafeArea;
    else if (name == "view") out = Space::View;
    else return false;
    return true;
}

const char* actionTypeName(ActionType type){
    switch (type) {
        case ActionType::Wait: return "wait";
        case ActionType::WaitUntil: return "wait until";
        case ActionType::Shot: return "shot";
        case ActionType::KeyDown: return "key down";
        case ActionType::KeyUp: return "key up";
        case ActionType::Text: return "text";
        case ActionType::TextUnits: return "text per unit";
        case ActionType::Ime: return "ime";
        case ActionType::ImeCommit: return "ime commit";
        case ActionType::ImeCancel: return "ime cancel";
        case ActionType::MouseEnter: return "mouse_enter";
        case ActionType::MouseLeave: return "mouse_leave";
        case ActionType::MouseMove: return "mouse move";
        case ActionType::MouseMoveBy: return "mouse move";
        case ActionType::MouseDown: return "mouse down";
        case ActionType::MouseUp: return "mouse up";
        case ActionType::MouseScroll: return "scroll";
        case ActionType::DragStart: return "drag start";
        case ActionType::DragMove: return "drag move";
        case ActionType::DragEnd: return "drag end";
        case ActionType::TouchStart: return "touch start";
        case ActionType::TouchMove: return "touch move";
        case ActionType::TouchEnd: return "touch end";
        case ActionType::TouchCancel: return "touch cancel";
        case ActionType::SwipeStart: return "swipe start";
        case ActionType::SwipeMove: return "swipe move";
        case ActionType::SwipeEnd: return "swipe end";
        case ActionType::GamepadConnect: return "gamepad connect";
        case ActionType::GamepadDisconnect: return "gamepad disconnect";
        case ActionType::GamepadButtonDown: return "gamepad down";
        case ActionType::GamepadButtonUp: return "gamepad up";
        case ActionType::GamepadAxis: return "gamepad axis";
        case ActionType::ActionDown: return "action down";
        case ActionType::ActionUp: return "action up";
        case ActionType::Axis: return "axis";
        case ActionType::AxisDelta: return "axis delta";
        case ActionType::InputActionPress: return "input action press";
        case ActionType::InputActionDown: return "input action down";
        case ActionType::InputActionUp: return "input action up";
        case ActionType::ReleaseAll: return "release all";
        case ActionType::AppPause: return "app pause";
        case ActionType::AppResume: return "app resume";
        case ActionType::AppFocusLost: return "app focus_lost";
        case ActionType::AppFocusGained: return "app focus_gained";
        case ActionType::AppLowMemory: return "app low_memory";
        case ActionType::AppHide: return "app hide";
        case ActionType::AppShow: return "app show";
        case ActionType::AppViewLost: return "app view_lost";
        case ActionType::AppViewRestored: return "app view_restored";
        case ActionType::AppOrientation: return "app orientation";
        case ActionType::AppBack: return "app back";
        case ActionType::GamepadCursor: return "gamepad cursor";
    }
    return "?";
}

bool Checkpoint::operator==(const Checkpoint& other) const{
    if (frame != other.frame || hash != other.hash || values != other.values || probes.size() != other.probes.size())
        return false;
    for (size_t i = 0; i < probes.size(); i++) {
        if (probes[i].x != other.probes[i].x || probes[i].y != other.probes[i].y || probes[i].z != other.probes[i].z)
            return false;
    }
    return true;
}

bool ScriptPoint::operator==(const ScriptPoint& other) const{
    return given == other.given && space == other.space && x == other.x && y == other.y && target == other.target;
}

bool ScriptAction::operator==(const ScriptAction& other) const{
    return frame == other.frame && framesAfter == other.framesAfter && msAfter == other.msAfter && event == other.event &&
           type == other.type && code == other.code && device == other.device && mods == other.mods && count == other.count &&
           string == other.string && value == other.value && value2 == other.value2 && point == other.point && to == other.to;
}

uint32_t ParsedScript::lastFrame() const{
    if (header.kind == ScriptKind::Recording) {
        uint32_t last = std::max(header.frames, header.checkpoints.empty() ? 0u : header.checkpoints.back().frame);
        for (const ScriptAction& action : actions)
            last = std::max(last, action.frame);
        return last;
    }
    uint64_t frames = 0;
    for (const ScriptAction& action : actions)
        frames += action.type == ActionType::WaitUntil ? static_cast<uint64_t>(std::max(action.count, 0)) : action.framesAfter;
    return static_cast<uint32_t>(std::min<uint64_t>(frames, UINT32_MAX));
}

const std::string& ParsedScript::stringAt(int32_t index) const{
    static const std::string empty;
    return index >= 0 && static_cast<size_t>(index) < strings.size() ? strings[static_cast<size_t>(index)] : empty;
}

bool HeldInput::empty() const{
    if (!keys.empty() || !mouseButtons.empty() || !touches.empty() || !gamepads.empty() || !actions.empty() ||
        !managerActions.empty() || !axes.empty())
        return false;
    for (const auto& [id, buttons] : gamepadButtons) {
        if (!buttons.empty())
            return false;
    }
    return true;
}

// ================================================================== parsing

bool parseEventList(const ScriptJson& events, const EventParseOptions& options, ParsedScript& out, std::string& error,
                    size_t& errorEvent){
    errorEvent = 0;
    if (!events.is_array()) {
        error = "events must be an array of event objects";
        return false;
    }
    if (events.size() > SCRIPT_MAX_EVENTS) {
        errorEvent = SCRIPT_MAX_EVENTS;
        error = "more than " + std::to_string(SCRIPT_MAX_EVENTS) + " events: split the file";
        return false;
    }
    uint32_t lastStamp = 0;
    for (size_t i = 0; i < events.size(); i++) {
        const ScriptJson& event = events[i];
        const size_t first = out.actions.size();
        uint32_t stamp = 0;
        const bool stamped = event.is_object() && event.contains("frame");
        if (options.recording) {
            uint64_t value = 0;
            if (!stamped || !nonNegativeWhole(event["frame"], value)) {
                errorEvent = i;
                error = "a recording's event needs frame, the update frame it arrived on (a whole number >= 0)";
                return false;
            }
            if (value > UINT32_MAX) {
                errorEvent = i;
                error = "frame " + std::to_string(value) + " is past the last frame a recording can hold";
                return false;
            }
            stamp = static_cast<uint32_t>(value);
            if (stamp < lastStamp) {
                errorEvent = i;
                error = "frame " + std::to_string(stamp) + " goes back in time (the event before is at frame " +
                        std::to_string(lastStamp) + "); a recording's frames never decrease";
                return false;
            }
            lastStamp = stamp;
        } else if (stamped && !options.live) {
            errorEvent = i;
            error = "a script's events carry no frame (each follows the one before); stamped events need kind \"recording\"";
            return false;
        }
        if (!buildEvent(event, static_cast<uint32_t>(i), options, out, error)) {
            errorEvent = i;
            out.actions.resize(first);
            return false;
        }
        if (options.recording) {
            // an event's steps from its stamp on: a held press ends its hold frames later
            uint32_t frame = stamp;
            for (size_t k = first; k < out.actions.size(); k++) {
                out.actions[k].frame = frame;
                frame += out.actions[k].framesAfter;
            }
        }
        out.eventLabels.push_back(eventLabel(lowerText(stringOf(event, "type")), event));
    }
    out.events = events.size();
    return true;
}

bool parseEvents(const ScriptJson& events, Space defaultSpace, int gapFrames, int fixedFps, ParsedScript& out, std::string& error){
    EventParseOptions options;
    options.space = defaultSpace;
    options.gapFrames = gapFrames;
    options.fixedFps = fixedFps;
    size_t index = 0;
    clearScript(out);
    if (!parseEventList(events, options, out, error, index)) {
        error = "events[" + std::to_string(index) + "]: " + error;
        clearScript(out);
        return false;
    }
    return true;
}

namespace {

    bool parseHeader(const ScriptJson& root, ScriptHeader& header, std::string& error){
        if (!root.is_object()) {
            error = "an input file is a JSON object with format \"jasperengine-input\", version 1, kind and events";
            return false;
        }
        if (stringOf(root, "format") != SCRIPT_FORMAT) {
            error = "format must be \"jasperengine-input\" (is this an input file?)";
            return false;
        }
        if (!root.contains("version") || !root["version"].is_number_integer()) {
            error = "version must be 1";
            return false;
        }
        const int version = root["version"].get<int>();
        if (version > SCRIPT_VERSION) {
            error = "version " + std::to_string(version) + " was made by a newer engine (this one reads version " +
                    std::to_string(SCRIPT_VERSION) + "): update the engine or record the file again";
            return false;
        }
        if (version < 1) {
            error = "version must be 1";
            return false;
        }
        const std::string kind = stringOf(root, "kind");
        if (kind == "recording") {
            header.kind = ScriptKind::Recording;
        } else if (kind == "script") {
            header.kind = ScriptKind::Script;
        } else {
            error = kind.empty() ? "kind is missing: \"recording\" (frame-stamped events) or \"script\" (steps)"
                                 : "kind must be \"recording\" or \"script\", not \"" + kind + "\"";
            return false;
        }
        const bool recording = header.kind == ScriptKind::Recording;
        if (root.contains("fixed_fps")) {
            if (!root["fixed_fps"].is_number_integer() || root["fixed_fps"].get<int>() < 1 || root["fixed_fps"].get<int>() > 1000) {
                error = "fixed_fps must be a whole number of frames a second (1..1000)";
                return false;
            }
            header.fixedFps = root["fixed_fps"].get<int>();
        }
        header.space = recording ? Space::Normalized : Space::Canvas;
        if (root.contains("space")) {
            const std::string space = stringOf(root, "space");
            if (space == "view") {
                error = "space \"view\" is the editor's window and cannot be stored: use normalized, canvas or safe_area";
                return false;
            }
            if (!parseSpace(space, header.space)) {
                error = "space must be canvas, normalized or safe_area";
                return false;
            }
        }
        if (root.contains("gap_frames")) {
            if (!root["gap_frames"].is_number_integer()) {
                error = "gap_frames must be a whole number of frames";
                return false;
            }
            header.gapFrames = std::clamp(root["gap_frames"].get<int>(), 0, 1000);
        }
        if (root.contains("start")) {
            const std::string start = stringOf(root, "start");
            if (start != "play" && start != "now") {
                error = "start must be \"play\" (from Play's start) or \"now\"";
                return false;
            }
            header.fromPlayStart = start == "play";
        }
        if (root.contains("seed")) {
            uint64_t seed = 0;
            if (!nonNegativeWhole(root["seed"], seed) || seed > UINT32_MAX) {
                error = "seed must be a 32-bit whole number";
                return false;
            }
            header.hasSeed = true;
            header.seed = static_cast<uint32_t>(seed);
        }
        header.scene = stringOf(root, "scene");
        header.platform = stringOf(root, "platform");
        header.engineVersion = stringOf(root, "engine_version");
        if (root.contains("canvas")) {
            const ScriptJson& c = root["canvas"];
            if (!c.is_array() || c.size() != 2 || !c[0].is_number() || !c[1].is_number()) {
                error = "canvas must be [width, height]";
                return false;
            }
            header.canvas = Vector2(c[0].get<float>(), c[1].get<float>());
        }
        header.simulateTouch = root.contains("simulate_touch") && root["simulate_touch"].is_boolean() && root["simulate_touch"].get<bool>();
        header.truncated = root.contains("truncated") && root["truncated"].is_boolean() && root["truncated"].get<bool>();
        if (root.contains("frames")) {
            uint64_t frames = 0;
            if (!nonNegativeWhole(root["frames"], frames) || frames > UINT32_MAX) {
                error = "frames must be the recording's length in frames (a whole number, 0 or more)";
                return false;
            }
            header.frames = static_cast<uint32_t>(frames);
        }
        if (root.contains("probes")) {
            for (const ScriptJson& p : root["probes"]) {
                if (!p.is_string()) {
                    error = "probes must be GameObject paths";
                    return false;
                }
                header.probes.push_back(p.get<std::string>());
            }
        }
        if (root.contains("checkpoints")) {
            if (!root["checkpoints"].is_array()) {
                error = "checkpoints must be an array";
                return false;
            }
            for (const ScriptJson& c : root["checkpoints"]) {
                Checkpoint checkpoint;
                const std::string hash = stringOf(c, "hash");
                uint64_t frame = 0;
                if (!c.is_object() || !c.contains("frame") || !nonNegativeWhole(c["frame"], frame) || hash.empty() || hash.size() > 16 ||
                    hash.find_first_not_of("0123456789abcdef") != std::string::npos) {
                    error = "each checkpoint needs frame and hash (16 hex digits)";
                    return false;
                }
                checkpoint.frame = static_cast<uint32_t>(std::min<uint64_t>(frame, UINT32_MAX));
                checkpoint.hash = std::stoull(hash, nullptr, 16);
                if (c.contains("probes") && c["probes"].is_array()) {
                    for (const ScriptJson& p : c["probes"]) {
                        if (!p.is_array() || p.size() != 3 || !p[0].is_number() || !p[1].is_number() || !p[2].is_number()) {
                            error = "a checkpoint's probes are [x, y, z] positions";
                            return false;
                        }
                        checkpoint.probes.emplace_back(p[0].get<float>(), p[1].get<float>(), p[2].get<float>());
                    }
                }
                if (c.contains("values") && c["values"].is_object())
                    checkpoint.values = c["values"];
                header.checkpoints.push_back(std::move(checkpoint));
            }
        }
        static const char* const known[] = {"format", "version", "kind", "start", "fixed_fps", "space", "gap_frames", "seed",
                                            "scene", "platform", "engine_version", "canvas", "simulate_touch", "probes",
                                            "checkpoints", "truncated", "frames", "events"};
        for (auto it = root.begin(); it != root.end(); ++it) {
            if (std::find(std::begin(known), std::end(known), it.key()) == std::end(known))
                header.extra[it.key()] = it.value();
        }
        return true;
    }

}

namespace {
    bool parseScriptChecked(const ScriptJson& root, ParsedScript& out, std::string& error);
}

bool parseScriptJson(const ScriptJson& root, ParsedScript& out, std::string& error){
    // the checks above each read; a value of an unexpected type that one missed must still fail clean
    try {
        return parseScriptChecked(root, out, error);
    } catch (const std::exception& e) {
        clearScript(out);
        error = std::string("not a valid input file: ") + e.what();
        return false;
    }
}

namespace {

bool parseScriptChecked(const ScriptJson& root, ParsedScript& out, std::string& error){
    clearScript(out);
    out.header = ScriptHeader();
    if (!parseHeader(root, out.header, error))
        return false;
    if (!root.contains("events")) {
        error = "events is missing: an array of event objects";
        return false;
    }
    EventParseOptions options;
    options.space = out.header.space;
    options.gapFrames = out.header.kind == ScriptKind::Script ? out.header.gapFrames : 0;
    options.fixedFps = out.header.fixedFps;
    options.recording = out.header.kind == ScriptKind::Recording;
    size_t index = 0;
    if (!parseEventList(root["events"], options, out, error, index)) {
        if (root["events"].is_array() && root["events"].size() <= SCRIPT_MAX_EVENTS)
            error = "events[" + std::to_string(index) + "]: " + error;
        clearScript(out);
        return false;
    }
    return true;
}

}

bool parseScript(std::string_view json, ParsedScript& out, std::string& error){
    clearScript(out);
    if (json.size() > SCRIPT_MAX_BYTES) {
        error = "the file is larger than 64 MB: split it";
        return false;
    }
    ScriptJson root = ScriptJson::parse(json.begin(), json.end(), nullptr, false);
    if (root.is_discarded()) {
        error = "not valid JSON (a truncated or hand-edited file?)";
        return false;
    }
    return parseScriptJson(root, out, error);
}

// ================================================================== writing

namespace {

    // typed text the recorder kept may not be valid UTF-8: replaced, never thrown
    std::string dumpLine(const nlohmann::ordered_json& value){
        return value.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace);
    }

    void putPoint(ScriptJson& event, const ScriptPoint& point, const ParsedScript& script){
        if (!point.given)
            return;
        if (point.target >= 0) {
            const ScriptTarget& target = script.targets[static_cast<size_t>(point.target)];
            event["target"] = target.selector;
            if (!target.document.is_null())
                event["document"] = target.document;
            return;
        }
        event["x"] = scriptRound4(point.x);
        event["y"] = scriptRound4(point.y);
        if (point.space != script.header.space)
            event["space"] = spaceName(point.space);
    }

    bool actionEvent(const ScriptAction& a, const ParsedScript& script, ScriptJson& e, std::string& error){
        const std::string& text = script.stringAt(a.string);
        switch (a.type) {
            case ActionType::Wait: e = {{"type", "wait"}, {"frames", std::max<uint32_t>(a.framesAfter, 1)}}; break;
            case ActionType::WaitUntil: e = {{"type", "wait"}, {"until", text}, {"frames", a.count}}; break;
            case ActionType::Shot: e = {{"type", "shot"}, {"name", text}}; break;
            case ActionType::KeyDown:
            case ActionType::KeyUp:
                e = {{"type", "key"}, {"key", InputControlNames::keyName(a.code)}, {"action", a.type == ActionType::KeyDown ? "down" : "up"}};
                if (a.mods) e["mods"] = modsJson(a.mods);
                break;
            case ActionType::Text: e = {{"type", "text"}, {"text", text}}; break;
            case ActionType::TextUnits: e = {{"type", "text"}, {"text", text}, {"per_unit", true}}; break;
            case ActionType::Ime: e = {{"type", "ime"}, {"text", text}, {"cursor", a.count}}; break;
            case ActionType::ImeCommit: e = {{"type", "ime_commit"}, {"text", text}}; break;
            case ActionType::ImeCancel: e = {{"type", "ime_cancel"}}; break;
            case ActionType::MouseEnter: e = {{"type", "mouse_enter"}}; break;
            case ActionType::MouseLeave: e = {{"type", "mouse_leave"}}; break;
            case ActionType::MouseMove:
            case ActionType::MouseDown:
            case ActionType::MouseUp:
            case ActionType::MouseScroll: {
                const char* action = a.type == ActionType::MouseMove ? "move" : a.type == ActionType::MouseDown ? "down"
                                   : a.type == ActionType::MouseUp ? "up" : "scroll";
                e = {{"type", "mouse"}, {"action", action}};
                if (a.type == ActionType::MouseDown || a.type == ActionType::MouseUp)
                    e["button"] = InputControlNames::mouseButtonName(a.code);
                if (a.type == ActionType::MouseScroll) {
                    e["dx"] = a.value2;
                    e["dy"] = a.value;
                }
                if (a.mods) e["mods"] = modsJson(a.mods);
                putPoint(e, a.point, script);
                break;
            }
            case ActionType::TouchStart:
            case ActionType::TouchMove:
            case ActionType::TouchEnd:
            case ActionType::TouchCancel: {
                const char* action = a.type == ActionType::TouchStart ? "start" : a.type == ActionType::TouchMove ? "move"
                                   : a.type == ActionType::TouchEnd ? "end" : "cancel";
                e = {{"type", "touch"}, {"action", action}, {"pointer", a.device}};
                putPoint(e, a.point, script);
                break;
            }
            case ActionType::GamepadConnect: e = {{"type", "gamepad"}, {"action", "connect"}, {"gamepad", a.device}, {"name", text}}; break;
            case ActionType::GamepadDisconnect: e = {{"type", "gamepad"}, {"action", "disconnect"}, {"gamepad", a.device}}; break;
            case ActionType::GamepadButtonDown:
            case ActionType::GamepadButtonUp:
                e = {{"type", "gamepad"}, {"gamepad", a.device}, {"button", InputControlNames::gamepadButtonName(a.code)},
                     {"action", a.type == ActionType::GamepadButtonDown ? "down" : "up"}};
                break;
            case ActionType::GamepadAxis:
                e = {{"type", "gamepad"}, {"action", "axis"}, {"gamepad", a.device}, {"axis", InputControlNames::gamepadAxisName(a.code)},
                     {"value", a.value}};
                break;
            case ActionType::ActionDown: e = {{"type", "action"}, {"name", text}, {"phase", "down"}}; break;
            case ActionType::ActionUp: e = {{"type", "action"}, {"name", text}, {"phase", "up"}}; break;
            case ActionType::Axis: e = {{"type", "axis"}, {"name", text}, {"value", a.value}}; break;
            case ActionType::AxisDelta: e = {{"type", "axis_delta"}, {"name", text}, {"delta", a.value}}; break;
            case ActionType::InputActionPress:
                e = {{"type", "input_action"}, {"name", text}, {"action", a.code ? "value" : "press"}, {"hold_frames", a.count},
                     {"value", ScriptJson::array({a.value, a.value2})}};
                break;
            case ActionType::InputActionDown:
                e = {{"type", "input_action"}, {"name", text}, {"action", "down"}, {"value", ScriptJson::array({a.value, a.value2})}};
                break;
            case ActionType::InputActionUp: e = {{"type", "input_action"}, {"name", text}, {"action", "up"}}; break;
            case ActionType::ReleaseAll: e = {{"type", "release_all"}}; break;
            case ActionType::AppPause: e = {{"type", "app"}, {"action", "pause"}}; break;
            case ActionType::AppResume: e = {{"type", "app"}, {"action", "resume"}}; break;
            case ActionType::AppFocusLost: e = {{"type", "app"}, {"action", "focus_lost"}}; break;
            case ActionType::AppFocusGained: e = {{"type", "app"}, {"action", "focus_gained"}}; break;
            case ActionType::AppLowMemory: e = {{"type", "app"}, {"action", "low_memory"}}; break;
            case ActionType::AppHide: e = {{"type", "app"}, {"action", "hide"}}; break;
            case ActionType::AppShow: e = {{"type", "app"}, {"action", "show"}}; break;
            case ActionType::AppViewLost: e = {{"type", "app"}, {"action", "view_lost"}}; break;
            case ActionType::AppViewRestored: e = {{"type", "app"}, {"action", "view_restored"}}; break;
            case ActionType::AppOrientation: e = {{"type", "app"}, {"action", "orientation"}, {"orientation", text}}; break;
            case ActionType::AppBack:
                e = {{"type", "app"}, {"action", "back"}};
                if (a.code == static_cast<int>(BackSource::BackButton)) e["source"] = "back_button";
                break;
            case ActionType::GamepadCursor:
            case ActionType::MouseMoveBy:
            case ActionType::DragStart:
            case ActionType::DragMove:
            case ActionType::DragEnd:
            case ActionType::SwipeStart:
            case ActionType::SwipeMove:
            case ActionType::SwipeEnd:
                error = std::string("a ") + actionTypeName(a.type) + " step is part of a script's event: a recording holds single events";
                return false;
        }
        return true;
    }

}

bool writeScript(const ParsedScript& script, std::string& out, std::string& error){
    const ScriptHeader& h = script.header;
    const bool recording = h.kind == ScriptKind::Recording;
    nlohmann::ordered_json head;
    head["format"] = SCRIPT_FORMAT;
    head["version"] = SCRIPT_VERSION;
    head["kind"] = recording ? "recording" : "script";
    if (recording)
        head["start"] = h.fromPlayStart ? "play" : "now";
    head["fixed_fps"] = h.fixedFps;
    head["space"] = spaceName(h.space);
    if (!recording)
        head["gap_frames"] = h.gapFrames;
    if (h.hasSeed)
        head["seed"] = h.seed;
    if (!h.scene.empty())
        head["scene"] = h.scene;
    if (h.canvas.x > 0 && h.canvas.y > 0)
        head["canvas"] = {h.canvas.x, h.canvas.y};
    if (recording)
        head["simulate_touch"] = h.simulateTouch;
    if (!h.platform.empty())
        head["platform"] = h.platform;
    if (!h.engineVersion.empty())
        head["engine_version"] = h.engineVersion;
    if (!h.probes.empty())
        head["probes"] = h.probes;
    if (recording)
        head["truncated"] = h.truncated;
    if (recording && h.frames > 0)
        head["frames"] = h.frames;
    for (auto it = h.extra.begin(); it != h.extra.end(); ++it)
        head[it.key()] = it.value();

    std::string text = "{\n";
    for (auto it = head.begin(); it != head.end(); ++it)
        text += "  " + dumpLine(nlohmann::ordered_json(it.key())) + ": " + dumpLine(it.value()) + ",\n";

    text += "  \"events\": [";
    bool first = true;
    for (size_t i = 0; i < script.actions.size(); i++) {
        const ScriptAction& a = script.actions[i];
        // an input action press's trailing gap belongs to its event
        if (a.type == ActionType::Wait && i > 0 && script.actions[i - 1].type == ActionType::InputActionPress &&
            script.actions[i - 1].event == a.event)
            continue;
        ScriptJson e;
        if (!actionEvent(a, script, e, error))
            return false;
        nlohmann::ordered_json line;
        if (recording)
            line["frame"] = a.frame;
        for (auto it = e.begin(); it != e.end(); ++it) {
            if (it.key() == "type")
                line["type"] = it.value();
        }
        for (auto it = e.begin(); it != e.end(); ++it) {
            if (it.key() != "type")
                line[it.key()] = it.value();
        }
        text += (first ? "\n    " : ",\n    ") + dumpLine(line);
        first = false;
    }
    text += first ? "]" : "\n  ]";

    if (!h.checkpoints.empty()) {
        text += ",\n  \"checkpoints\": [";
        for (size_t i = 0; i < h.checkpoints.size(); i++) {
            const Checkpoint& c = h.checkpoints[i];
            char hash[17];
            std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(c.hash));
            nlohmann::ordered_json line;
            line["frame"] = c.frame;
            line["hash"] = hash;
            nlohmann::ordered_json probes = nlohmann::ordered_json::array();
            for (const Vector3& p : c.probes)
                probes.push_back({scriptRound4(p.x), scriptRound4(p.y), scriptRound4(p.z)});
            line["probes"] = probes;
            line["values"] = c.values;
            text += (i == 0 ? "\n    " : ",\n    ") + dumpLine(line);
        }
        text += "\n  ]";
    }
    text += "\n}\n";
    out = std::move(text);
    return true;
}

// ================================================================== the player

namespace {

    // canvas units -> the Engine::system* coordinates (the engine's view, in framebuffer pixels)
    Vector2 canvasToEngine(Vector2 canvas){
        const Rect view = Engine::getViewRect();
        const double canvasW = std::max(1, Engine::getCanvasWidth());
        const double canvasH = std::max(1, Engine::getCanvasHeight());
        return Vector2(static_cast<float>(view.getX() + canvas.x / canvasW * view.getWidth()),
                       static_cast<float>(view.getY() + canvas.y / canvasH * view.getHeight()));
    }

    Vector2 clampToView(Vector2 point){
        const Rect view = Engine::getViewRect();
        return Vector2(std::clamp(point.x, view.getX(), view.getX() + std::max(0.0f, view.getWidth())),
                       std::clamp(point.y, view.getY(), view.getY() + std::max(0.0f, view.getHeight())));
    }

    // a point of a space -> canvas units; error when outside the game canvas
    bool toCanvas(Space space, double x, double y, Vector2& out, std::string& error){
        const Rect view = Engine::getViewRect();
        const double canvasW = Engine::getCanvasWidth();
        const double canvasH = Engine::getCanvasHeight();
        if (view.getWidth() <= 0 || view.getHeight() <= 0 || canvasW <= 0 || canvasH <= 0) {
            error = "The game view has no size yet.";
            return false;
        }
        double cx = x, cy = y;
        if (space == Space::Normalized) {
            cx = x * canvasW;
            cy = y * canvasH;
        } else if (space == Space::SafeArea) {
            const Rect safe = Screen::getSafeArea();
            cx = safe.getX() + x * safe.getWidth();
            cy = safe.getY() + y * safe.getHeight();
        } else if (space == Space::View) {
            cx = (x - view.getX()) / view.getWidth() * canvasW;
            cy = (y - view.getY()) / view.getHeight() * canvasH;
        }
        if (!Engine::isAllowEventsOutCanvas() && (cx < 0 || cy < 0 || cx > canvasW || cy > canvasH)) {
            error = "Point (" + std::to_string(x) + ", " + std::to_string(y) + ") in " + spaceName(space) +
                    " space is outside the game canvas (" + std::to_string((int)canvasW) + "x" + std::to_string((int)canvasH) +
                    "); the game would ignore it.";
            return false;
        }
        out = Vector2(static_cast<float>(cx), static_cast<float>(cy));
        return true;
    }

    // the engine's Lua state: `return <condition>` (or the condition as a chunk), truthy
    bool evalInEngineLua(const std::string& condition, bool& truthy, std::string& error){
        lua_State* L = LuaBinding::getLuaState();
        if (!L) {
            error = "no Lua state (is the game running?)";
            return false;
        }
        const int top = lua_gettop(L);
        const std::string expr = "return " + condition;
        if (luaL_loadbufferx(L, expr.c_str(), expr.size(), "=until", "t") != LUA_OK) {
            lua_settop(L, top);
            if (luaL_loadbufferx(L, condition.c_str(), condition.size(), "=until", "t") != LUA_OK) {
                error = "until is not valid Lua: " + LuaBinding::getLuaStackErrorString(L, -1);
                lua_settop(L, top);
                return false;
            }
        }
        if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
            error = "until failed: " + LuaBinding::getLuaStackErrorString(L, -1);
            lua_settop(L, top);
            return false;
        }
        truthy = lua_toboolean(L, -1) != 0;
        lua_settop(L, top);
        return true;
    }

}

ScriptPlayer::ScriptPlayer(std::shared_ptr<const ParsedScript> script, PlayerHost host, PlayOptions options)
    : script_(std::move(script)), host_(std::move(host)), options_(std::move(options)){
    held_ = options_.held ? options_.held : std::make_shared<HeldInput>();
    heldBefore_ = *held_;
    if (!script_)
        script_ = std::make_shared<ParsedScript>();
    // checkpoints compare only from a known start, on the platform that recorded them (R19)
    const ScriptHeader& h = script_->header;
    report_.comparable = h.kind == ScriptKind::Recording && h.fromPlayStart && !h.checkpoints.empty() && h.platform == platformTag();
}

ScriptPlayer::~ScriptPlayer(){
    if (options_.releaseAtEnd && !held_->empty())
        releaseHeld();
}

bool ScriptPlayer::isInjecting(){
    return injectingDepth.load(std::memory_order_relaxed) > 0;
}

std::string ScriptPlayer::errorText() const{
    if (!report_.failed)
        return std::string();
    const size_t event = report_.failedEvent;
    const std::string label = event < script_->eventLabels.size() ? script_->eventLabels[event] : std::string("event");
    return "Event " + std::to_string(event) + " (" + label + "): " + report_.error;
}

void ScriptPlayer::fail(const ScriptAction& action, const std::string& error){
    report_.failed = true;
    report_.error = error;
    report_.failedEvent = action.event;
    if (options_.releaseAtEnd)
        releaseHeld();
}

Vector2 ScriptPlayer::currentPointer(){
    if (held_->mousePosKnown)
        return held_->mousePos;
    Vector2 canvas = Input::getMousePosition();
    std::string error;
    Vector2 c;
    if (toCanvas(Space::Canvas, canvas.x, canvas.y, c, error))
        return host_.toEnginePoint ? host_.toEnginePoint(c) : canvasToEngine(c);
    return held_->mousePos;
}

bool ScriptPlayer::resolve(const ScriptAction& action, const ScriptPoint& point, Vector2& out, std::string& error, bool engineSpace){
    Vector2 canvas;
    if (point.target >= 0) {
        const ScriptTarget& target = script_->targets[static_cast<size_t>(point.target)];
        if (!host_.resolveTarget) {
            error = "target needs the htmlui module in this player";
            return false;
        }
        ScriptJson found;
        std::string why;
        if (!host_.resolveTarget(target.selector, target.document, canvas, why, &found) ||
            !toCanvas(Space::Canvas, canvas.x, canvas.y, canvas, why)) {
            error = "target \"" + target.selector + "\": " + why;
            return false;
        }
        report_.targets.push_back({action.event, target.selector,
                                   Vector2(std::round(canvas.x * 10.0f) / 10.0f, std::round(canvas.y * 10.0f) / 10.0f), found});
    } else if (!toCanvas(point.space, point.x, point.y, canvas, error)) {
        return false;
    }
    out = !engineSpace ? canvas : host_.toEnginePoint ? host_.toEnginePoint(canvas) : canvasToEngine(canvas);
    return true;
}

bool ScriptPlayer::runAction(const ScriptAction& a, std::string& error){
    InjectScope injecting;
    // what the interpreter forces counts as input (an agent's forced action), never game code's
    switch (a.type) {
        case ActionType::ActionDown: case ActionType::ActionUp: case ActionType::Axis: case ActionType::AxisDelta:
        case ActionType::InputActionPress: case ActionType::InputActionDown: case ActionType::InputActionUp:
            if (InputRecorder::isActive())
                InputRecorder::recordForced(a, *script_);
            break;
        default:
            break;
    }
    HeldInput& held = *held_;
    const std::string& text = script_->stringAt(a.string);
    auto pointerInjected = [this]() {
        if (host_.pointerInjected)
            host_.pointerInjected(true);
    };
    // Simulate Touch: the left button is a finger, as in the editor
    const bool asTouch = options_.simulateTouch && a.code == D_MOUSE_BUTTON_LEFT;
    auto moveTo = [&](Vector2 p) {
        pointerInjected();
        if (asTouch) {
            if (held.touches.count(0)) {
                Engine::systemTouchMove(0, p.x, p.y);
                held.touches[0] = p;
            }
        } else {
            Engine::systemMouseMove(p.x, p.y, a.mods);
        }
        held.mousePos = p;
        held.mousePosKnown = true;
    };
    auto pressAt = [&](Vector2 p) {
        pointerInjected();
        if (asTouch) {
            Engine::systemTouchStart(0, p.x, p.y);
            held.touches[0] = p;
        } else {
            Engine::systemMouseMove(p.x, p.y, a.mods);
            Engine::systemMouseDown(a.code, p.x, p.y, a.mods);
            held.mouseButtons.insert(a.code);
        }
        held.mousePos = p;
        held.mousePosKnown = true;
    };
    auto releaseAt = [&](Vector2 p) {
        pointerInjected();
        if (asTouch) {
            Engine::systemTouchEnd(0, p.x, p.y);
            held.touches.erase(0);
        } else {
            Engine::systemMouseUp(a.code, p.x, p.y, a.mods);
            held.mouseButtons.erase(a.code);
        }
        held.mousePos = p;
        held.mousePosKnown = true;
    };
    auto connect = [&](int pad, const std::string& name) {
        if (!held.gamepads.count(pad)) {
            Engine::systemGamepadConnect(pad, name);
            held.gamepads.insert(pad);
        }
    };
    auto actionSource = [this]() { return InputActions::namedSource(options_.actionSource); };
    auto actionExists = [&](const std::string& name) {
        if (InputActions::hasAction(name))
            return true;
        error = "no input action \"" + name + "\" (Project Settings > Input)";
        return false;
    };
    auto injectedExists = [&](const std::string& name) {
        if (InputActions::get(name))
            return true;
        error = "no Input Action " + name + " is loaded (get_input_state fields actions lists them)";
        return false;
    };
    auto lerp = [](Vector2 from, Vector2 to, float t) { return Vector2(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t); };

    Vector2 p;
    switch (a.type) {
        case ActionType::Wait:
        case ActionType::WaitUntil:
            return true;
        case ActionType::Shot:
            if (host_.shot)
                return host_.shot(text, report_.frames, error);
            return true;
        case ActionType::KeyDown:
            Engine::systemKeyDown(a.code, false, a.mods);
            held.keys.insert(a.code);
            return true;
        case ActionType::KeyUp:
            Engine::systemKeyUp(a.code, false, a.mods);
            held.keys.erase(a.code);
            return true;
        case ActionType::Text:
            Engine::systemTextInput(text);
            return true;
        case ActionType::TextUnits:
            // a character beyond U+FFFF arrives as two halves the engine must combine
            for (uint32_t cp : decodeUtf8(text)) {
                if (cp > 0xFFFF) {
                    const uint32_t value = cp - 0x10000;
                    Engine::systemCharInput(static_cast<char32_t>(0xD800 + (value >> 10)));
                    Engine::systemCharInput(static_cast<char32_t>(0xDC00 + (value & 0x3FF)));
                } else {
                    Engine::systemCharInput(static_cast<char32_t>(cp));
                }
            }
            return true;
        case ActionType::Ime:
            Engine::systemImeComposition(text, a.count);
            return true;
        case ActionType::ImeCommit:
            if (text.empty())
                Engine::systemImeCancel();
            else
                Engine::systemTextInput(text);
            return true;
        case ActionType::ImeCancel:
            Engine::systemImeCancel();
            return true;
        case ActionType::MouseEnter:
        case ActionType::MouseLeave:
            pointerInjected();
            if (a.type == ActionType::MouseLeave)
                Engine::systemMouseLeave();
            else
                Engine::systemMouseEnter();
            return true;
        case ActionType::MouseMove:
            if (!resolve(a, a.point, p, error))
                return false;
            moveTo(p);
            return true;
        case ActionType::MouseMoveBy: {
            const Rect view = Engine::getViewRect();
            const double canvasW = std::max(1, Engine::getCanvasWidth());
            const double canvasH = std::max(1, Engine::getCanvasHeight());
            // the event's space to engine pixels
            double sx = 1.0, sy = 1.0;
            if (a.point.space == Space::Canvas) {
                sx = view.getWidth() / canvasW;
                sy = view.getHeight() / canvasH;
            } else if (a.point.space == Space::Normalized) {
                sx = view.getWidth();
                sy = view.getHeight();
            } else if (a.point.space == Space::SafeArea) {
                const Rect safe = Screen::getSafeArea();
                sx = safe.getWidth() * view.getWidth() / canvasW;
                sy = safe.getHeight() * view.getHeight() / canvasH;
            }
            const Vector2 from = currentPointer();
            moveTo(Vector2(static_cast<float>(from.x + a.value * sx), static_cast<float>(from.y + a.value2 * sy)));
            return true;
        }
        case ActionType::MouseDown:
        case ActionType::MouseUp:
            // without a point: where the pointer is
            if (a.point.given) {
                if (!resolve(a, a.point, p, error))
                    return false;
            } else {
                p = currentPointer();
            }
            if (a.type == ActionType::MouseDown)
                pressAt(p);
            else
                releaseAt(p);
            return true;
        case ActionType::MouseScroll:
            if (a.point.given) {
                if (!resolve(a, a.point, p, error))
                    return false;
                moveTo(p);
            }
            Engine::systemMouseScroll(a.value2, a.value, a.mods);
            return true;
        case ActionType::DragStart:
        case ActionType::SwipeStart:
            // both ends are resolved when it starts (before it moves anything)
            if (!resolve(a, a.point, pathFrom_, error) || !resolve(a, a.to, pathTo_, error))
                return false;
            if (a.type == ActionType::DragStart) {
                pressAt(pathFrom_);
            } else {
                pointerInjected();
                Engine::systemTouchStart(a.device, pathFrom_.x, pathFrom_.y);
                held.touches[a.device] = pathFrom_;
            }
            return true;
        case ActionType::DragMove:
            moveTo(lerp(pathFrom_, pathTo_, a.value));
            return true;
        case ActionType::DragEnd:
            releaseAt(pathTo_);
            return true;
        case ActionType::SwipeMove:
            p = lerp(pathFrom_, pathTo_, a.value);
            pointerInjected();
            Engine::systemTouchMove(a.device, p.x, p.y);
            held.touches[a.device] = p;
            return true;
        case ActionType::TouchStart:
        case ActionType::TouchMove:
            if (!resolve(a, a.point, p, error))
                return false;
            pointerInjected();
            if (a.type == ActionType::TouchStart)
                Engine::systemTouchStart(a.device, p.x, p.y);
            else
                Engine::systemTouchMove(a.device, p.x, p.y);
            held.touches[a.device] = p;
            return true;
        case ActionType::TouchEnd:
        case ActionType::SwipeEnd:
            if (a.type == ActionType::SwipeEnd) {
                p = pathTo_;
            } else if (a.point.given) {
                if (!resolve(a, a.point, p, error))
                    return false;
            } else {
                // where the finger is
                p = held.touches.count(a.device) ? held.touches[a.device] : held.mousePos;
            }
            pointerInjected();
            Engine::systemTouchEnd(a.device, p.x, p.y);
            held.touches.erase(a.device);
            return true;
        case ActionType::TouchCancel:
            Engine::systemTouchCancel(a.device);
            held.touches.erase(a.device);
            return true;
        case ActionType::GamepadConnect:
            connect(a.device, text);
            return true;
        case ActionType::GamepadDisconnect:
            Engine::systemGamepadDisconnect(a.device);
            held.gamepads.erase(a.device);
            held.gamepadButtons.erase(a.device);
            held.gamepadAxes.erase(a.device);
            return true;
        case ActionType::GamepadAxis:
            if (a.count)
                connect(a.device, "MCP Virtual Gamepad");
            Engine::systemGamepadAxisMove(a.device, a.code, a.value);
            if (a.value != 0.0f)
                held.gamepadAxes[a.device].insert(a.code);
            else
                held.gamepadAxes[a.device].erase(a.code);
            return true;
        case ActionType::GamepadButtonDown:
            if (a.count)
                connect(a.device, "MCP Virtual Gamepad");
            Engine::systemGamepadButtonDown(a.device, a.code);
            held.gamepadButtons[a.device].insert(a.code);
            return true;
        case ActionType::GamepadButtonUp:
            Engine::systemGamepadButtonUp(a.device, a.code);
            held.gamepadButtons[a.device].erase(a.code);
            return true;
        case ActionType::ActionDown:
        case ActionType::ActionUp:
            if (!actionExists(text))
                return false;
            InputActions::setSourceValue(actionSource(), text, a.type == ActionType::ActionDown ? 1.0f : 0.0f, InputSourceKind::Agent);
            if (a.type == ActionType::ActionDown)
                held.managerActions.insert(text);
            else
                held.managerActions.erase(text);
            return true;
        case ActionType::Axis:
            if (!actionExists(text))
                return false;
            InputActions::setSourceValue(actionSource(), text, a.value, InputSourceKind::Agent);
            if (a.value != 0.0f)
                held.axes[text] = a.value;
            else
                held.axes.erase(text);
            return true;
        case ActionType::AxisDelta:
            if (!actionExists(text))
                return false;
            InputActions::addSourceDelta(actionSource(), text, a.value, InputSourceKind::Agent);
            return true;
        case ActionType::InputActionPress:
            if (!injectedExists(text))
                return false;
            InputActions::inject(text, Vector2(a.value, a.value2), a.count);
            return true;
        case ActionType::InputActionDown:
            if (!injectedExists(text))
                return false;
            InputActions::inject(text, Vector2(a.value, a.value2), -1);
            held.actions.insert(text);
            return true;
        case ActionType::InputActionUp:
            if (!injectedExists(text))
                return false;
            InputActions::releaseInjected(text);
            held.actions.erase(text);
            return true;
        case ActionType::ReleaseAll:
            releaseHeld();
            return true;
        case ActionType::AppPause: Engine::systemPause(); return true;
        case ActionType::AppResume: Engine::systemResume(); return true;
        case ActionType::AppFocusLost: Engine::systemFocus(false); return true;
        case ActionType::AppFocusGained: Engine::systemFocus(true); return true;
        case ActionType::AppLowMemory: Engine::systemLowMemory(); return true;
        case ActionType::AppHide:
        case ActionType::AppShow: {
            const bool hidden = a.type == ActionType::AppHide;
            if (host_.appHidden) {
                error = host_.appHidden(hidden);
                return error.empty();
            }
            Engine::systemVisibilityChanged(!hidden);
            return true;
        }
        case ActionType::AppViewLost:
        case ActionType::AppViewRestored:
            if (!host_.appViewLost) {
                error = "app view_lost / view_restored simulate a lost GPU view: they run in the editor's Play";
                return false;
            }
            error = host_.appViewLost(a.type == ActionType::AppViewLost);
            return error.empty();
        case ActionType::AppOrientation:
            if (!host_.appOrientation) {
                // a recording's turn was the device's: replayed where none can be simulated, the
                // rest of it still runs
                if (script_->header.kind == ScriptKind::Recording) {
                    if (!orientationSkipped_)
                        Log::warn("Input replay: the recorded orientation turns are skipped (%s at frame %u first): only the editor's Game view simulates a device",
                                  text.c_str(), a.frame);
                    orientationSkipped_ = true;
                    return true;
                }
                error = "app orientation turns a simulated device: it runs in the editor's Game view";
                return false;
            }
            error = host_.appOrientation(text);
            return error.empty();
        case ActionType::AppBack: {
            Engine::systemBack(a.code == static_cast<int>(BackSource::BackButton) ? BackSource::BackButton : BackSource::Agent);
            static const char* const routes[] = {"escape", "key", "document", "game"};
            report_.backs.push_back({a.event, routes[static_cast<int>(Engine::getLastBackRoute())]});
            return true;
        }
        case ActionType::GamepadCursor: {
            if (!Engine::isGamepadCursorShown()) {
                error = "no document has Gamepad Cursor on: set_properties UIDocument gamepadCursor true";
                return false;
            }
            Vector2 canvas;
            if (!resolve(a, a.point, canvas, error, false))
                return false;
            GamepadCursor::setPosition(canvas);
            return true;
        }
    }
    return true;
}

bool ScriptPlayer::beforeFrame(uint64_t frameIndex){
    if (finished())
        return false;
    // a shot being taken: the run goes on after it
    if (host_.busy && host_.busy())
        return true;
    const uint64_t elapsed = started_ && frameIndex > lastFrame_ ? frameIndex - lastFrame_ : 0;
    started_ = true;
    lastFrame_ = frameIndex;
    const std::vector<ScriptAction>& actions = script_->actions;
    const bool recording = script_->header.kind == ScriptKind::Recording;

    auto evalUntil = [&](const ScriptAction& a, bool& truthy) {
        std::string error;
        const std::string& condition = script_->stringAt(a.string);
        const bool ok = host_.evalCondition ? host_.evalCondition(condition, truthy, error) : evalInEngineLua(condition, truthy, error);
        if (!ok)
            fail(a, error);
        return ok;
    };

    if (recording) {
        report_.frames = frameIndex;
    } else if (pendingFrames_ > 0 || msPending_) {
        if (elapsed == 0)
            return true;
        report_.frames += elapsed;
        pendingFrames_ = pendingFrames_ > elapsed ? pendingFrames_ - elapsed : 0;
        if (pendingFrames_ > 0 || (msPending_ && std::chrono::steady_clock::now() < msDeadline_))
            return true;
        msPending_ = false;
    }
    if (waitingUntil_) {
        if (elapsed == 0)
            return true;
        const ScriptAction& a = actions[next_];
        untilFrames_ += elapsed;
        if (!recording)
            report_.frames += elapsed;
        bool truthy = false;
        if (!evalUntil(a, truthy))
            return false;
        if (!truthy) {
            if (untilFrames_ >= static_cast<uint64_t>(std::max(a.count, 1))) {
                fail(a, "until timed out after " + std::to_string(a.count) + " frames: " + script_->stringAt(a.string));
                return false;
            }
            return true;
        }
        waitingUntil_ = false;
        report_.actionsDone = ++next_;
    }

    while (next_ < actions.size()) {
        const ScriptAction& a = actions[next_];
        if (recording && a.frame > frameIndex)
            return true;
        if (a.type == ActionType::WaitUntil) {
            bool truthy = false;
            if (!evalUntil(a, truthy))
                return false;
            if (!truthy) {
                waitingUntil_ = true;
                untilFrames_ = 0;
                return true;
            }
            report_.actionsDone = ++next_;
            continue;
        }
        std::string error;
        const bool ok = runAction(a, error);
        report_.actionsDone = ++next_;
        if (!ok) {
            fail(a, error);
            return false;
        }
        if (a.type == ActionType::Shot) {
            report_.shots.push_back({script_->stringAt(a.string), report_.frames});
            if (host_.busy && host_.busy()) {
                if (!recording)
                    pendingFrames_ = a.framesAfter;
                return true;
            }
        }
        if (recording)
            continue;
        if (a.msAfter > 0 && options_.msFixedFps > 0) {
            // milliseconds of game frames (each exactly 1/fixed_fps s), not of the wall clock, so
            // the input lasts the same frames however fast the machine runs
            const uint64_t msFrames = static_cast<uint64_t>(std::ceil(a.msAfter * options_.msFixedFps / 1000.0 - 1e-9));
            pendingFrames_ = std::max<uint64_t>({a.framesAfter, msFrames, 1});
            return true;
        }
        if (a.msAfter > 0) {
            pendingFrames_ = std::max<uint32_t>(a.framesAfter, 1);
            msPending_ = true;
            msDeadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(a.msAfter);
            return true;
        }
        if (a.framesAfter > 0) {
            pendingFrames_ = a.framesAfter;
            return true;
        }
    }
    // the last action's own frames (a trailing wait) are the run's too; a recording lasts to the
    // frame it was stopped on, and to its last checkpoint
    if (!recording && pendingFrames_ > 0)
        return true;
    const ScriptHeader& header = script_->header;
    if (recording && frameIndex < std::max(header.frames, header.checkpoints.empty() ? 0u : header.checkpoints.back().frame))
        return true;
    report_.done = true;
    if (options_.releaseAtEnd)
        releaseHeld();
    return false;
}

void ScriptPlayer::afterUpdate(uint64_t frameIndex){
    const std::vector<Checkpoint>& checkpoints = script_->header.checkpoints;
    if (finished() || checkpoints.empty())
        return;
    // the frame that just updated is the (frameIndex + 1)th: a checkpoint's frame counts frames run
    const uint64_t frame = frameIndex + 1;
    while (nextCheckpoint_ < checkpoints.size() && checkpoints[nextCheckpoint_].frame < frame)
        nextCheckpoint_++;
    if (nextCheckpoint_ >= checkpoints.size() || checkpoints[nextCheckpoint_].frame != frame)
        return;
    const Checkpoint& recorded = checkpoints[nextCheckpoint_++];
    // later checkpoints are not compared: past the first difference they all differ
    if (!report_.comparable || report_.divergedAtFrame >= 0)
        return;
    const Checkpoint replayed = sampleCheckpoint(static_cast<uint32_t>(frame), script_->header.probes);
    ScriptJson differences;
    report_.compared++;
    if (compareCheckpoint(recorded, replayed, script_->header.probes, differences))
        return;
    report_.divergedAtFrame = static_cast<int64_t>(frame);
    report_.differences = differences;
    const ScriptJson line = {{"frame", frame}, {"differences", differences}};
    Log::print("[input] diverged %s", line.dump(-1, ' ', false, ScriptJson::error_handler_t::replace).c_str());
    if (options_.stopOnDivergence && !script_->actions.empty())
        fail(script_->actions[std::min(next_, script_->actions.size() - 1)], "the replay diverged at frame " + std::to_string(frame));
}

void releaseHeldInput(HeldInput& held, const std::string& actionSource){
    InjectScope injecting;
    for (int key : held.keys)
        Engine::systemKeyUp(key, false, 0);
    const Vector2 pos = clampToView(held.mousePos);
    for (int button : held.mouseButtons)
        Engine::systemMouseUp(button, pos.x, pos.y, 0);
    for (const auto& [pointer, point] : held.touches) {
        const Vector2 p = clampToView(point);
        Engine::systemTouchEnd(pointer, p.x, p.y);
    }
    for (const auto& [pad, buttons] : held.gamepadButtons) {
        for (int button : buttons)
            Engine::systemGamepadButtonUp(pad, button);
    }
    for (const auto& [pad, axes] : held.gamepadAxes) {
        for (int axis : axes)
            Engine::systemGamepadAxisMove(pad, axis, 0.0f);
    }
    for (int pad : held.gamepads)
        Engine::systemGamepadDisconnect(pad);
    for (const std::string& action : held.actions)
        InputActions::releaseInjected(action);
    // the Input Manager's actions and axes it held (action / axis / axis_delta)
    InputActions::clearSource(InputActions::namedSource(actionSource));
    held = HeldInput();
}

void ScriptPlayer::releaseHeld(){
    releaseHeldInput(*held_, options_.actionSource);
    heldBefore_ = HeldInput();
    if (host_.pointerInjected)
        host_.pointerInjected(false);
}

bool ScriptPlayer::cutShort(){
    if (finished())
        return false;
    report_.done = true;
    waitingUntil_ = false;
    pendingFrames_ = 0;
    msPending_ = false;
    if (next_ >= script_->actions.size())
        return false;   // only the trailing gap was left
    InjectScope injecting;
    HeldInput& held = *held_;
    const HeldInput& before = heldBefore_;
    std::vector<int> keys, buttons, touches;
    for (int key : held.keys)
        if (!before.keys.count(key)) keys.push_back(key);
    for (int key : keys) {
        Engine::systemKeyUp(key, false, 0);
        held.keys.erase(key);
    }
    const Vector2 pos = clampToView(held.mousePos);
    for (int button : held.mouseButtons)
        if (!before.mouseButtons.count(button)) buttons.push_back(button);
    for (int button : buttons) {
        Engine::systemMouseUp(button, pos.x, pos.y, 0);
        held.mouseButtons.erase(button);
    }
    for (const auto& [pointer, point] : held.touches)
        if (!before.touches.count(pointer)) touches.push_back(pointer);
    for (int pointer : touches) {
        const Vector2 p = clampToView(held.touches[pointer]);
        Engine::systemTouchEnd(pointer, p.x, p.y);
        held.touches.erase(pointer);
    }
    for (auto& [pad, padButtons] : held.gamepadButtons) {
        std::vector<int> up;
        auto it = before.gamepadButtons.find(pad);
        for (int b : padButtons)
            if (it == before.gamepadButtons.end() || !it->second.count(b)) up.push_back(b);
        for (int b : up) {
            Engine::systemGamepadButtonUp(pad, b);
            padButtons.erase(b);
        }
    }
    // the input actions it forced down are released
    std::vector<std::string> actions;
    for (const std::string& name : held.actions)
        if (!before.actions.count(name)) actions.push_back(name);
    for (const std::string& name : actions) {
        InputActions::releaseInjected(name);
        held.actions.erase(name);
    }
    // the Input Manager's actions it pressed are released, the axes it moved go back to where they were
    const uintptr_t source = InputActions::namedSource(options_.actionSource);
    std::vector<std::string> managerActions;
    for (const std::string& name : held.managerActions)
        if (!before.managerActions.count(name)) managerActions.push_back(name);
    for (const std::string& name : managerActions) {
        InputActions::setSourceValue(source, name, 0.0f, InputSourceKind::Agent);
        held.managerActions.erase(name);
    }
    std::vector<std::string> axes;
    for (const auto& [name, value] : held.axes) {
        auto it = before.axes.find(name);
        if (it == before.axes.end() || it->second != value) axes.push_back(name);
    }
    for (const std::string& name : axes) {
        auto it = before.axes.find(name);
        const float value = it == before.axes.end() ? 0.0f : it->second;
        InputActions::setSourceValue(source, name, value, InputSourceKind::Agent);
        if (value != 0.0f)
            held.axes[name] = value;
        else
            held.axes.erase(name);
    }
    return true;
}

// ================================================================== the runner

namespace {

    struct RunnerEntry {
        std::shared_ptr<ScriptPlayer> player;
        std::function<void(ScriptPlayer&)> onEnded;
        uint64_t frames = 0;
        bool ranThisFrame = false;
    };

    struct RunnerState {
        std::vector<RunnerEntry> entries;
        std::atomic<bool> active{false};
    };

    RunnerState& runner(){
        static RunnerState s;
        return s;
    }

}

bool ScriptRunner::isActive(){
    return runner().active.load(std::memory_order_relaxed);
}

void ScriptRunner::start(std::shared_ptr<ScriptPlayer> player, std::function<void(ScriptPlayer&)> onEnded){
    if (!player)
        return;
    RunnerState& s = runner();
    s.entries.push_back({std::move(player), std::move(onEnded), 0, false});
    s.active.store(true, std::memory_order_relaxed);
}

bool ScriptRunner::stop(const ScriptPlayer* player){
    RunnerState& s = runner();
    for (auto it = s.entries.begin(); it != s.entries.end(); ++it) {
        if (it->player.get() == player) {
            std::shared_ptr<ScriptPlayer> keep = it->player;
            s.entries.erase(it);
            s.active.store(!s.entries.empty(), std::memory_order_relaxed);
            keep->cutShort();
            keep->releaseHeld();
            return true;
        }
    }
    return false;
}

void ScriptRunner::stopAll(){
    RunnerState& s = runner();
    std::vector<RunnerEntry> entries;
    entries.swap(s.entries);
    s.active.store(false, std::memory_order_relaxed);
    for (RunnerEntry& entry : entries) {
        entry.player->cutShort();
        entry.player->releaseHeld();
    }
}

std::vector<std::shared_ptr<ScriptPlayer>> ScriptRunner::running(){
    std::vector<std::shared_ptr<ScriptPlayer>> out;
    for (const RunnerEntry& entry : runner().entries)
        out.push_back(entry.player);
    return out;
}

int64_t ScriptRunner::frameOf(const ScriptPlayer* player){
    for (const RunnerEntry& entry : runner().entries) {
        if (entry.player.get() == player)
            return static_cast<int64_t>(entry.frames);
    }
    return -1;
}

void ScriptRunner::beforeFrame(bool running){
    RunnerState& s = runner();
    std::vector<RunnerEntry> ended;
    // a copy: an ended run's callback may start another
    std::vector<std::shared_ptr<ScriptPlayer>> players;
    for (RunnerEntry& entry : s.entries) {
        entry.ranThisFrame = false;
        // a recording's stamps are update frames: a paused app's frames do not count for it
        if (!running && entry.player->script().header.kind == ScriptKind::Recording)
            continue;
        entry.ranThisFrame = true;
        entry.player->beforeFrame(entry.frames);
    }
    for (auto it = s.entries.begin(); it != s.entries.end();) {
        if (it->player->finished()) {
            ended.push_back(std::move(*it));
            it = s.entries.erase(it);
        } else {
            ++it;
        }
    }
    s.active.store(!s.entries.empty(), std::memory_order_relaxed);
    // frames advance once the update ran (afterUpdate), or now for a paused app's frame
    if (!running) {
        for (RunnerEntry& entry : s.entries)
            if (entry.ranThisFrame) entry.frames++;
    }
    for (RunnerEntry& entry : ended) {
        if (entry.onEnded)
            entry.onEnded(*entry.player);
    }
}

void ScriptRunner::afterUpdate(){
    RunnerState& s = runner();
    for (RunnerEntry& entry : s.entries) {
        if (!entry.ranThisFrame)
            continue;
        entry.player->afterUpdate(entry.frames);
        entry.frames++;
        entry.ranThisFrame = false;
    }
}

}

#endif // JASPERENGINE_INPUT_SCRIPTS_ENABLED
