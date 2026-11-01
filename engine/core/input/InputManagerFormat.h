// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef INPUTMANAGERFORMAT_H
#define INPUTMANAGERFORMAT_H

// Unity's Input Manager as an action map of the Input Actions (docs/specs/S22-gameplay.md,
// "Upstream absorption"). Its flat list of named virtual axes and buttons, each binding one Input
// Manager entry ("Key or Mouse Button", "Joystick Axis", "Mouse Movement" with gravity,
// sensitivity, dead, snap and invert), is the JSON shape of the project.yaml input node older
// editors wrote, of MCP input.manager_actions, of Lua Input.getBindings / setBindings and of the
// exported binding overrides:
//
//   {"version": 1, "actions": [{name, type: axis|button|vector2, description?, press_point?,
//     hold_time?, deadzone?, bindings: [
//       {kind: buttons, positive: [control names], negative: [...], gamepad?, gravity, sensitivity, dead, snap?, invert?}
//     | {kind: gamepad_axis, axis: left_x..right_trigger, gamepad: any|0-15, sensitivity, dead, invert?}
//     | {kind: mouse_delta, axis: x|y|wheel, sensitivity, invert?}
//     | {kind: binding, binding: {an .inputactions binding: path or composite, ...}}]}]}
//
// It converts both ways, losslessly, with the "Input Manager" map of an .inputactions asset:
// buttons are axis1d composites with Input Manager smoothing (their lists as the roles' parts),
// a gamepad axis is gamepad/<axis> with its dead zone and sensitivity as deadzone and scale, mouse
// movement is mouse/delta/x, mouse/delta/y or mouse/scroll/y; any other binding of the map keeps
// its .inputactions JSON (kind binding). The map takes Unity's eleven defaults when a project has
// no actions asset.

#include "Export.h"
#include "input/InputActionAsset.h"
#include "input/InputControlNames.h"

#include <string>
#include <vector>

namespace jasperengine {

    // Button: a value 0..1 (a negative list is ignored); Axis: -1..1; Vector2: an .inputactions VECTOR2
    enum class InputManagerActionType { Axis, Button, Vector2 };

    enum class InputManagerBindingKind { Buttons, GamepadAxis, MouseDelta, Binding };

    enum class MouseDeltaAxis { X, Y, Wheel };

    struct JASPERENGINE_API InputManagerBinding {
        InputManagerBindingKind kind = InputManagerBindingKind::Buttons;
        // Buttons: keys, mouse buttons and gamepad buttons; any held one counts
        std::vector<InputControl> positive;
        std::vector<InputControl> negative;
        // -1: any gamepad (the map's), else 0..15 (gamepad buttons of Buttons, GamepadAxis)
        int gamepad = -1;
        int axis = 0;                                     // GamepadAxis: D_GAMEPAD_AXIS_*
        MouseDeltaAxis mouseAxis = MouseDeltaAxis::X;     // MouseDelta
        // Unity's knobs. Buttons: the value moves to the target at sensitivity/s and back to 0 at
        // gravity/s (<= 0: at once), |value| < dead reads 0, snap jumps through 0 on a reversal.
        // GamepadAxis: dead zone (rescaled: the output starts at 0), sensitivity scales.
        // MouseDelta: sensitivity scales the canvas px (wheel steps). invert flips the sign.
        float gravity = 3.0f;
        float sensitivity = 3.0f;
        float dead = 0.001f;
        bool snap = false;
        bool invert = false;
        // Binding: the .inputactions binding (compact JSON, no id)
        std::string binding;
    };

    // A named virtual axis or button: what Unity calls a virtual axis and Godot / the Input
    // System an action
    struct JASPERENGINE_API InputManagerAction {
        std::string name;
        std::string description;
        InputManagerActionType type = InputManagerActionType::Axis;
        // |value| at which an analog source (a gamepad axis, an on-screen stick) counts as pressed
        float pressPoint = 0.5f;
        // the .inputactions extras (0 and none: the Input Manager's)
        float holdTime = 0.0f;
        std::optional<float> deadzone;
        std::vector<InputManagerBinding> bindings;
    };

    class JASPERENGINE_API InputManagerFormat {
    public:
        // the map of an .inputactions asset that holds the Input Manager's actions
        static constexpr const char* MAP_NAME = "Input Manager";

        // ---- Unity's Input Manager defaults (Horizontal, Vertical, Fire1-3, Jump, Mouse X/Y,
        // Mouse ScrollWheel, Submit, Cancel)
        static std::vector<InputManagerAction> defaults();
        // the asset Play and a game use when the project has no actions asset: the defaults as
        // the Input Manager map
        static InputActionAssetDesc defaultsAsset();
        // a new binding of that kind with Unity's defaults (keyboard 3/3/0.001, joystick 0.19/1, mouse 0.1)
        static InputManagerBinding makeBinding(InputManagerBindingKind kind);

        // ---- the JSON shape
        // {"version": 1, "actions": [...]} or the bare list; false + error (out unchanged) when invalid
        static bool parseActionsJson(const std::string& json, std::vector<InputManagerAction>& out, std::string* error);
        static std::string actionsToJson(const std::vector<InputManagerAction>& actions, int indent = -1);
        // one action's bindings ([{kind: buttons, positive: [...]}, ...]); `action` names it in errors
        static bool parseBindingsJson(const std::string& json, std::vector<InputManagerBinding>& out, std::string* error,
                                      const std::string& action = "");
        static std::string bindingsToJson(const std::vector<InputManagerBinding>& bindings);
        // "" when valid, else the first problem ("action \"Dash\" binding 1: ...")
        static std::string validate(const std::vector<InputManagerAction>& actions);
        static std::string validateBindings(const std::vector<InputManagerBinding>& bindings, const std::string& action);

        static const char* typeName(InputManagerActionType type);
        static const char* kindName(InputManagerBindingKind kind);
        static const char* mouseAxisName(MouseDeltaAxis axis);

        // ---- the Input Manager map of an .inputactions asset
        // the actions as the map (consume off, so it takes no control from the asset's other maps;
        // ids derived from the names, so the same actions give the same asset)
        static InputActionMapDesc toMap(const std::vector<InputManagerAction>& actions, const std::string& mapName = MAP_NAME);
        static std::vector<InputManagerAction> fromMap(const InputActionMapDesc& map);
        static InputActionDesc toAction(const InputManagerAction& action, const std::string& idSeed);
        static InputManagerAction fromAction(const InputActionDesc& action);
        static InputBindingDesc toBinding(const InputManagerBinding& binding, const std::string& idSeed);
        static InputManagerBinding fromBinding(const InputBindingDesc& binding);
        // the asset with its Input Manager map replaced by these actions (added after the others
        // when it has none)
        static void setInAsset(InputActionAssetDesc& asset, const std::vector<InputManagerAction>& actions);
    };

}

#endif // INPUTMANAGERFORMAT_H
