// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef INPUTACTIONS_H
#define INPUTACTIONS_H

// Input Actions (docs/specs/S22-gameplay.md R8-R18, docs/mcp/guide/input.md): action maps of
// actions whose values come from bindings to controls, evaluated once per frame right after
// Input's latch (Engine::systemDraw), in map priority order. A map that uses a control
// consumes it for the maps below it; an exclusive map blocks them all; a binding that needs
// modifiers suppresses the plain binding of the same key; a press the UI consumed does not
// count for maps that ignore it. Callbacks fire during the evaluation, before the fixed steps.
// Rebinding writes overrides (binding id -> path) that persist in UserSettings.

#include "Export.h"
#include "input/InputActionAsset.h"
#include "math/Vector2.h"
#include "util/FunctionSubscribe.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {

    class InputAction;
    class InputBinding;

    enum class InputActionPhase : int {
        WAITING = 0,    // at rest
        STARTED,        // its value left 0 (not performed yet: under the press point, or holding)
        PERFORMED,      // pressed (value >= press point), held for holdTime when it has one
        CANCELED        // released (in the window it was released; WAITING after)
    };

    // What a callback gets: the action, its value at the edge, and consume()
    class JASPERENGINE_API InputActionEvent {
    public:
        InputAction* action = nullptr;
        InputActionPhase phase = InputActionPhase::WAITING;
        float value = 0.0f;
        Vector2 vector;
        bool consumed = false;

        // the controls that drove the action reach no lower map's callbacks or state this window
        void consume();
        InputAction* getAction() const { return action; }
        InputActionPhase getPhase() const { return phase; }
        float getValue() const { return value; }
        Vector2 getVector2() const { return vector; }
        bool isConsumed() const { return consumed; }
    };

    class JASPERENGINE_API InputAction {
        friend class InputActions;
        friend struct InputActionsRuntime;

    private:
        std::string mapName;
        std::string name;
        InputActionType type = InputActionType::BUTTON;
        Vector2 value;
        bool pressed = false;
        bool pressedThisFrame = false;
        bool releasedThisFrame = false;
        // the fixed window's edges (each seen by one fixed step, as Input's)
        bool fixedPressed = false;
        bool fixedReleased = false;
        float heldTime = 0.0f;
        InputActionPhase phase = InputActionPhase::WAITING;
        int consumedByMap = -1;     // the map whose use of a control took it this window

    public:
        float readValue() const;            // BUTTON: 0..1, AXIS: -1..1, VECTOR2: the length
        Vector2 readVector2() const;
        bool isPressed() const;
        // pressed in this frame's window (also a tap: pressed and released inside it); inside a
        // fixed step, in the fixed window
        bool wasPressedThisFrame() const;
        bool wasReleasedThisFrame() const;
        float getHeldTime() const;          // seconds pressed (0 when not)
        InputActionPhase getPhase() const;
        const std::string& getName() const { return name; }
        const std::string& getMapName() const { return mapName; }
        std::string getPath() const { return mapName + "/" + name; }
        InputActionType getType() const { return type; }
        // the map that took one of its controls (or blocked it, exclusive) this window ("" none)
        std::string getConsumedBy() const;
        // its bindings, in order (a composite's parts: InputBinding::getParts)
        std::vector<InputBinding> getBindings() const;

        FunctionSubscribe<void(InputActionEvent&)> onStarted;
        FunctionSubscribe<void(InputActionEvent&)> onPerformed;
        FunctionSubscribe<void(InputActionEvent&)> onCanceled;
    };

    // A binding of the loaded asset by id (scripts: action:getBindings(), InputActions.getBinding)
    class JASPERENGINE_API InputBinding {
    private:
        std::string id;

    public:
        InputBinding() = default;
        explicit InputBinding(std::string id): id(std::move(id)) {}

        const std::string& getId() const { return id; }
        bool isValid() const;
        bool isComposite() const;
        // the effective control path (its override, else the asset's); "" for a composite
        std::string getPath() const;
        bool hasOverride() const;
        std::vector<std::string> getGroups() const;
        std::vector<InputBinding> getParts() const;
        // "Space", "Ctrl+S", "Left Stick", "W/A/S/D"
        std::string getDisplayName() const;
    };

    // An action map by name (scripts: InputActions.getMap("Menu"):enable())
    class JASPERENGINE_API InputActionMap {
    private:
        std::string name;

    public:
        InputActionMap() = default;
        explicit InputActionMap(std::string name): name(std::move(name)) {}

        const std::string& getName() const { return name; }
        bool isValid() const;
        bool isEnabled() const;
        void setEnabled(bool enabled);
        void enable() { setEnabled(true); }
        void disable() { setEnabled(false); }
        int getPriority() const;
        void setPriority(int priority);
        // -1: any pad
        int getGamepad() const;
        void setGamepad(int gamepadId);
        std::vector<InputAction*> getActions() const;
    };

    struct JASPERENGINE_API InputRebindOptions {
        std::vector<std::string> groups;        // only controls of these schemes (empty: any)
        std::vector<std::string> exclude;       // path patterns ('*' wildcard), e.g. "mouse/delta*"
        std::string cancel = "keyboard/escape"; // the control that cancels ("" none)
        float timeout = 0.0f;                   // seconds, 0 = none
    };

    enum class InputRebindStatus : int {
        COMPLETED = 0,
        CANCELED,
        TIMED_OUT
    };

    struct JASPERENGINE_API InputRebindResult {
        InputRebindStatus status = InputRebindStatus::CANCELED;
        std::string action;      // "Map/Action"
        std::string bindingId;
        std::string path;        // the new control path (COMPLETED)
    };

    // Where rebinding overrides are kept: UserSettings by default; tests use their own
    class JASPERENGINE_API InputOverrideStore {
    public:
        virtual ~InputOverrideStore() = default;
        virtual std::string read(const std::string& key) = 0;
        virtual void write(const std::string& key, const std::string& value) = 0;
        virtual void remove(const std::string& key) = 0;
    };

    // A binding as a conflict list or a debugger shows it
    struct JASPERENGINE_API InputBindingInfo {
        std::string action;      // "Map/Action"
        std::string id;
        std::string path;        // with its override applied
        std::vector<std::string> groups;
    };

    class JASPERENGINE_API InputActions {
    public:
        // ---- assets
        // Loads an .inputactions asset (a path the engine reads, as other assets) and its
        // overrides, replacing the loaded one; maps start as their asset says. False + a log
        // when it cannot be read
        static bool load(const std::string& path);
        // the same from text (the editor applies its edits live during Play)
        static bool loadFromText(const std::string& path, const std::string& text);
        // the project's default asset (Project Settings > Input): loaded at game start and Play
        static bool loadDefault(const std::string& path);
        // an edit of the loaded asset: the maps keep their state (enabled, priority, gamepad)
        static bool reloadText(const std::string& text);
        static void unload();
        static bool isLoaded();
        static const std::string& getAssetPath();
        // the loaded asset, overrides not applied
        static const InputActionAssetDesc& getAsset();

        // the default deadzone of AXIS and VECTOR2 actions without their own (0.15)
        static void setDefaultDeadzone(float deadzone);
        static float getDefaultDeadzone();

        // ---- actions and maps
        static InputAction* get(const std::string& mapSlashAction);
        static InputActionMap getMap(const std::string& name);
        static InputBinding getBinding(const std::string& id);
        static std::vector<InputAction*> getActions();
        static std::vector<std::string> getMapNames();
        static void enableMap(const std::string& map, bool enabled);
        static bool isMapEnabled(const std::string& map);
        static void setMapPriority(const std::string& map, int priority);
        static int getMapPriority(const std::string& map);
        // the pad a map reads: -1 any (the default); any-pad axes take the largest magnitude
        static void setGamepad(const std::string& map, int gamepadId);
        static int getGamepad(const std::string& map);

        // ---- script input
        // virtual/<name>: a scalar (x) or vec2 an on-screen stick or button sets
        static void setVirtual(const std::string& name, Vector2 value);
        static void setVirtual(const std::string& name, float value);
        static Vector2 getVirtual(const std::string& name);
        // forces an action's value for `windows` frames at top priority (0: a tap, pressed and
        // released in the next window; -1: until releaseInjected)
        static void inject(const std::string& mapSlashAction, Vector2 value, int windows);
        static void releaseInjected(const std::string& mapSlashAction);

        // ---- rebinding (the result is an override, not an asset edit)
        static bool startRebind(const std::string& mapSlashAction, const std::string& bindingId,
                                const InputRebindOptions& options, std::function<void(const InputRebindResult&)> onComplete);
        static void cancelRebind();
        static bool isRebinding();
        // the bindings of enabled maps in the binding's groups with the same (overridden) path
        static std::vector<InputBindingInfo> getConflicts(const std::string& mapSlashAction, const std::string& bindingId);
        // the effective path: its override, else the asset's
        static std::string getBindingPath(const std::string& bindingId);
        static void setOverride(const std::string& bindingId, const std::string& path);
        static void removeOverride(const std::string& bindingId);
        static bool saveOverrides();
        static bool loadOverrides();
        // clears the overrides and their stored copy
        static void resetOverrides();
        static std::string getOverridesKey();
        static void setOverrideStore(std::shared_ptr<InputOverrideStore> store);   // null: UserSettings

        // ---- display
        // "Space", "Ctrl+S", "Left Stick", "W/A/S/D" (gamepad names follow the connected pad:
        // Cross, Circle... on a PlayStation pad)
        static std::string getBindingDisplayName(const std::string& bindingId);
        static std::string getControlDisplayName(const std::string& path, int mods = 0);

        // ---- S25: a control path some binding of an enabled map uses (game shortcuts first)
        static bool isControlBound(const std::string& path);

        // ---- Engine only
        // the frame's evaluation, right after Input::beginFrame(); dt: the unscaled frame time
        static void evaluate(float dt);
        // after each fixed step (Input::endFixedStep)
        static void endFixedStep();
        // Play start and stop: maps back to the asset's state, injections, virtual controls and
        // a rebind dropped
        static void resetSession();

        // the debugger's view (the Input Actions window, get_input_state)
        struct MapState {
            std::string name;
            int priority = 0;
            bool enabled = false;
            int gamepad = -1;
            bool blocked = false;    // an exclusive map above it was enabled this window
        };
        static std::vector<MapState> getMapStates();

    private:
        static void evaluateWindow(float dt);
    };

}

#endif // INPUTACTIONS_H
