// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#ifndef InputActions_h
#define InputActions_h

#include "Export.h"
#include "Input.h"
#include "input/InputControlNames.h"
#include "math/Vector2.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine {

    // Button: a value 0..1 (a negative list is ignored); Axis: -1..1
    enum class InputActionType { Axis, Button };

    // One Unity Input Manager entry: "Key or Mouse Button", "Joystick Axis" or "Mouse Movement"
    enum class InputBindingKind { Buttons, GamepadAxis, MouseDelta };

    enum class MouseDeltaAxis { X, Y, Wheel };

    // Who feeds a virtual source: an on-screen control (UI), a script, an agent (MCP send_input)
    enum class InputSourceKind { Control, Script, Agent };

    struct InputBinding {
        InputBindingKind kind = InputBindingKind::Buttons;
        // Buttons: keys, mouse buttons and gamepad buttons; any held one counts
        std::vector<InputControl> positive;
        std::vector<InputControl> negative;
        // -1: any gamepad, else 0..15 (gamepad buttons of Buttons, GamepadAxis)
        int gamepad = -1;
        int axis = D_GAMEPAD_AXIS_LEFT_X;                 // GamepadAxis
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
    };

    // A named virtual axis or button (Project Settings > Input): what Unity calls a virtual
    // axis and Godot / the Input System an action
    struct InputAction {
        std::string name;
        std::string description;
        InputActionType type = InputActionType::Axis;
        // |value| at which an analog source (a gamepad axis, an on-screen stick) counts as pressed
        float pressPoint = 0.5f;
        std::vector<InputBinding> bindings;
    };

    // The input actions of the game: one set per process. Input::beginFrame updates them once per
    // frame (unscaled time: they work at time scale 0); the queries read that frame's values,
    // plus the virtual sources set since (their values count at once, their presses next frame).
    // Combining: bounded sources (button bindings after smoothing, gamepad axes, on-screen sticks
    // and buttons, simulated axes) are summed then clamped to [-1, 1] ([0, 1] for a Button
    // action); delta sources (mouse movement, touch-area drags, simulated deltas) are added
    // unclamped on top. An unknown action name logs one error and reads 0 / false.
    class JASPERENGINE_API InputActions {
    public:
        // ---- the definition (the project's, or Unity's Input Manager defaults)
        static std::vector<InputAction> defaults();
        static void loadDefaults();
        // replaces the definition (keeps the runtime overrides and states of the names that remain)
        static void setActions(const std::vector<InputAction>& actions);
        static const std::vector<InputAction>& getActions();
        // {"version": 1, "actions": [...]} or the bare list; false + error (nothing changes) when invalid
        static bool loadJson(const std::string& json, std::string* error);
        // the definition, as loadJson reads it
        static std::string toJson();

        // ---- the JSON shape shared by project.yaml, MCP (input.actions) and the exports
        static bool parseActionsJson(const std::string& json, std::vector<InputAction>& out, std::string* error);
        static std::string actionsToJson(const std::vector<InputAction>& actions, int indent = -1);
        // one action's bindings ([{kind: buttons, positive: [...]}, ...]); `action` names it in errors
        static bool parseBindingsJson(const std::string& json, std::vector<InputBinding>& out, std::string* error,
                                      const std::string& action = "");
        static std::string bindingsToJson(const std::vector<InputBinding>& bindings);
        // "" when valid, else the first problem ("action \"Dash\" binding 1: ...")
        static std::string validate(const std::vector<InputAction>& actions);
        static std::string validateBindings(const std::vector<InputBinding>& bindings, const std::string& action);

        static const char* typeName(InputActionType type);
        static const char* kindName(InputBindingKind kind);
        static const char* mouseAxisName(MouseDeltaAxis axis);
        // a new binding of that kind with Unity's defaults (keyboard 3/3/0.001, joystick 0.19/1, mouse 0.1)
        static InputBinding makeBinding(InputBindingKind kind);

        // ---- per frame (Input::beginFrame / endFrame)
        static void update(double unscaledDeltaTime, const InputSuppression& suppression = InputSuppression());
        static void endFrame();
        // the actions whose button went down / up in this frame's update (Engine.onButtonDown/Up)
        static const std::vector<std::string>& getFrameButtonDowns();
        static const std::vector<std::string>& getFrameButtonUps();

        // ---- queries
        static float getAxis(const std::string& name);
        static float getAxisRaw(const std::string& name);
        static bool getButton(const std::string& name);
        static bool getButtonDown(const std::string& name);
        static bool getButtonUp(const std::string& name);
        // (getAxis(x), getAxis(y)) of the bounded sources only, length clamped to 1
        static Vector2 getVector(const std::string& xName, const std::string& yName);
        static bool hasAction(const std::string& name);
        static std::vector<std::string> getActionNames();
        // a disabled action reads 0 / false and keeps no edges; enabling it starts from rest
        static void setActionEnabled(const std::string& name, bool enabled);
        static bool isActionEnabled(const std::string& name);

        // ---- virtual sources (on-screen controls, scripts, MCP agents). An owner is any unique
        // address (a component, an element, a script instance) or namedSource("mcp"). A source
        // owned by a script instance goes when the instance is released (scene unload): the
        // engine clears the owner of every released script (Engine::removeSubscriptionsByTag).
        // bounded value, clamped to [-1, 1]; 0 removes it
        static void setSourceValue(uintptr_t owner, const std::string& action, float value,
                                   InputSourceKind kind = InputSourceKind::Control);
        // a delta for this frame only (until Input::endFrame), added to what the owner gave already
        static void addSourceDelta(uintptr_t owner, const std::string& action, float delta,
                                   InputSourceKind kind = InputSourceKind::Control);
        static void clearSource(uintptr_t owner);
        static void clearSources(InputSourceKind kind);
        static void clearAllSources();
        // a stable owner for a name ("mcp", "script", a script's own name): never a real address
        static uintptr_t namedSource(const std::string& name);
        static bool isNamedSource(uintptr_t owner);

        // ---- runtime rebinding: overrides layered over the definition (settings screens)
        static bool setBindings(const std::string& action, const std::vector<InputBinding>& bindings,
                                std::string* error = nullptr);
        // the bindings in use (the override, else the definition's)
        static std::vector<InputBinding> getBindings(const std::string& action);
        static bool hasBindingOverride(const std::string& action);
        // "" = every action
        static void resetBindings(const std::string& action = "");
        // {"version": 1, "bindings": {"Jump": [...]}}: the overrides only
        static std::string exportOverrides();
        // replaces the overrides; actions the game no longer has are skipped; false + error
        // (nothing changes) when the text or a binding is invalid
        static bool importOverrides(const std::string& json, std::string* error);

        // Play stop / start in the editor: overrides, enabled flags, sources and smoothing go
        static void resetRuntimeState();

        // ---- live state (Project Settings > Input during Play, MCP get_play_state)
        struct State {
            float value = 0.0f;
            float raw = 0.0f;
            bool pressed = false;
            bool enabled = true;
        };
        static bool getState(const std::string& name, State& out);
    };

}

#endif /* InputActions_h */
