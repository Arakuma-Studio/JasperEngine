// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Gameplay (docs/specs/S22-gameplay.md): the input state the game reads (levels, the frame's
// edges and deltas, touches, gamepads, pointer lock). The guide is docs/mcp/guide/input.md.
#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "Engine.h"
#include "Input.h"
#include "input/InputControlPath.h"

#include <cmath>
#include <set>
#include <string>

namespace jasperengine::editor::mcp {

namespace {

using namespace schema;

double rounded(double value) {
    return std::round(value * 1000.0) / 1000.0;
}

Json vec2Json(const Vector2& v) {
    return Json::array({rounded(v.x), rounded(v.y)});
}

const char* touchPhaseName(TouchPhase phase) {
    switch (phase) {
        case TouchPhase::BEGAN: return "began";
        case TouchPhase::MOVED: return "moved";
        case TouchPhase::STATIONARY: return "stationary";
        case TouchPhase::ENDED: return "ended";
        case TouchPhase::CANCELED: return "canceled";
    }
    return "stationary";
}

const char* pointerLockName(PointerLockState state) {
    switch (state) {
        case PointerLockState::UNLOCKED: return "unlocked";
        case PointerLockState::PENDING: return "pending";
        case PointerLockState::LOCKED: return "locked";
    }
    return "unlocked";
}

Json modsJson(int mods) {
    Json list = Json::array();
    if (mods & D_MODIFIER_SHIFT) list.push_back("shift");
    if (mods & D_MODIFIER_CONTROL) list.push_back("ctrl");
    if (mods & D_MODIFIER_ALT) list.push_back("alt");
    if (mods & D_MODIFIER_SUPER) list.push_back("super");
    return list;
}

// held / down / up names of a range of controls, read from the frame's window
template <typename NameFn>
void controlEdgesJson(int first, int count, int controlBase, NameFn name, Json& held, Json& down, Json& up) {
    for (int i = first; i < first + count; i++) {
        const InputControlEdges edges = Input::getControlEdges(controlBase + i, false);
        if (edges.level) held.push_back(name(i));
        if (edges.presses > 0) down.push_back(name(i));
        if (edges.releases > 0) up.push_back(name(i));
    }
}

Json rawJson() {
    Json keysHeld = Json::array(), keysDown = Json::array(), keysUp = Json::array();
    controlEdgesJson(0, Input::KEY_CONTROLS, 0, [](int key) { return InputControlNames::keyName(key); },
                     keysHeld, keysDown, keysUp);
    Json buttonsHeld = Json::array(), buttonsDown = Json::array(), buttonsUp = Json::array();
    controlEdgesJson(0, Input::MOUSE_CONTROLS, Input::MOUSE_CONTROL_BASE,
                     [](int button) { return InputControlNames::mouseButtonName(button); },
                     buttonsHeld, buttonsDown, buttonsUp);
    return {
        {"keys", {{"held", keysHeld}, {"down", keysDown}, {"up", keysUp}}},
        {"mouse", {
            {"position", vec2Json(Input::getMousePosition())},
            {"delta", vec2Json(Input::getMouseDelta())},
            {"scroll_delta", vec2Json(Input::getMouseScrollDelta())},
            {"held", buttonsHeld}, {"down", buttonsDown}, {"up", buttonsUp},
            {"entered", Input::isMouseEntered()}
        }},
        {"any_key_down", Input::anyKeyDown()},
        {"modifiers", modsJson(Input::getModifiers())},
        {"pointer_lock", pointerLockName(Input::getPointerLockState())}
    };
}

Json touchesJson() {
    Json list = Json::array();
    for (size_t i = 0; i < Input::getTouchCount(); i++) {
        const Touch touch = Input::getTouch(i);
        list.push_back({
            {"pointer", touch.pointer},
            {"phase", touchPhaseName(touch.phase)},
            {"position", vec2Json(touch.position)},
            {"delta", vec2Json(touch.delta)},
            {"start_position", vec2Json(touch.startPosition)},
            {"time", rounded(touch.time)},
            {"began", touch.began},
            {"down", Input::getTouchDown(touch.pointer)},
            {"up", Input::getTouchUp(touch.pointer)}
        });
    }
    return list;
}

Json gamepadsJson() {
    Json list = Json::array();
    for (size_t i = 0; i < Input::numGamepads(); i++) {
        const int id = Input::getGamepadId(i);
        const int slot = Input::getGamepadSlot(id);
        Json held = Json::array(), down = Json::array(), up = Json::array();
        controlEdgesJson(0, Input::GAMEPAD_BUTTONS, Input::gamepadControl(slot, 0),
                         [](int button) { return InputControlNames::gamepadButtonName(button); }, held, down, up);
        Json axes = Json::object();
        for (int axis = 0; axis < Input::GAMEPAD_AXES; axis++) {
            axes[InputControlNames::gamepadAxisName(axis)] = rounded(Input::getGamepadAxis(id, axis));
        }
        list.push_back({{"gamepad", id}, {"name", Input::getGamepadName(id)}, {"held", held}, {"down", down},
                        {"up", up}, {"axes", axes}});
    }
    return list;
}

void getInputStateTool(const CallPtr& call) {
    const Json& args = call->args();
    std::set<std::string> fields;
    if (args.contains("fields") && args["fields"].is_array()) {
        for (const Json& field : args["fields"]) {
            if (field.is_string()) fields.insert(field.get<std::string>());
        }
    }
    const bool all = fields.empty();

    Json data = Json::object();
    data["game_frame"] = Engine::getGameFrames();
    std::string text = "Input of game frame " + std::to_string(Engine::getGameFrames());
    if (all || fields.count("raw")) {
        data["raw"] = rawJson();
        const Json& keys = data["raw"]["keys"];
        const Json& mouse = data["raw"]["mouse"];
        text += "\nkeys held " + keys["held"].dump() + " down " + keys["down"].dump() + " up " + keys["up"].dump();
        text += "\nmouse " + mouse["position"].dump() + " delta " + mouse["delta"].dump() + " scroll " +
                mouse["scroll_delta"].dump() + " held " + mouse["held"].dump() + " down " + mouse["down"].dump() +
                " up " + mouse["up"].dump() + "; pointer " + data["raw"]["pointer_lock"].get<std::string>();
    }
    if (all || fields.count("touches")) {
        data["touches"] = touchesJson();
        if (!data["touches"].empty()) text += "\ntouches " + data["touches"].dump();
    }
    if (all || fields.count("gamepads")) {
        data["gamepads"] = gamepadsJson();
        if (!data["gamepads"].empty()) text += "\ngamepads " + data["gamepads"].dump();
    }
    if (text.size() > 8000) text = text.substr(0, 8000) + " ... (see structuredContent)";
    call->ok(text, data);
}

} // namespace

void registerGameplayTools(McpRegistry& registry) {
    {
        ToolDef tool;
        tool.name = "get_input_state";
        tool.title = "Get Input State";
        tool.description =
            "What the game reads from Input at the last frame that ran: levels (held), the frame's edges "
            "(down/up: getKeyDown/getKeyUp), mouse delta and scroll delta, pointer lock, touches with "
            "phases, gamepads. auto_pause + send_input frames_after 0: the frame the input landed in (a tap: "
            "hold_frames 0).";
        tool.inputSchema = object({
            {"fields", array(stringEnum("", {"raw", "touches", "gamepads"}), "Default: all.")}
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = getInputStateTool;
        registry.addTool(std::move(tool));
    }
}

}
