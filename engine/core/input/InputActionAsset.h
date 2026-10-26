// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef INPUTACTIONASSET_H
#define INPUTACTIONASSET_H

// The .inputactions asset (docs/specs/S22-gameplay.md R8, R44): JSON with "version": 1 holding
// action maps -> actions -> bindings. The data model, its JSON form and the edit operations of
// the Input Actions window and the MCP edit_input_actions tool. Unknown keys at every level are
// written back as they were, and the output is deterministic (the same model, the same bytes).

#include "Export.h"
#include "json.hpp"

#include <optional>
#include <string>
#include <vector>

namespace jasperengine {

    using InputJson = nlohmann::ordered_json;

    enum class InputActionType : int {
        BUTTON = 0,     // pressed at value >= pressPoint
        AXIS,           // a scalar in [-1, 1]
        VECTOR2         // a vector of length <= 1 (deltas: unclamped)
    };

    enum class InputCompositeType : int {
        NONE = 0,
        AXIS_1D,        // parts negative, positive
        DPAD_2D         // parts up, down, left, right
    };

    struct JASPERENGINE_API InputBindingDesc {
        std::string id;                     // 8 hex digits, stable across edits and reorders
        std::string path;                   // a control path (InputControlPath); empty for a composite
        float scale = 1.0f;
        bool invert = false;
        std::optional<float> deadzone;      // overrides the action's (analog controls)
        int requiredMods = 0;               // D_MODIFIER_* that must be held
        int forbiddenMods = 0;              // D_MODIFIER_* that must not be held
        std::vector<std::string> groups;    // control schemes: "Keyboard&Mouse", "Gamepad", "Touch"
        InputCompositeType composite = InputCompositeType::NONE;
        bool normalize = true;              // DPAD_2D: the vector's length is at most 1
        // a composite's parts, in their order (negative/positive, up/down/left/right)
        std::vector<std::pair<std::string, InputBindingDesc>> parts;
        InputJson passthrough = InputJson::object();

        bool isComposite() const { return composite != InputCompositeType::NONE; }
        bool inGroup(const std::string& group) const;
    };

    struct JASPERENGINE_API InputActionDesc {
        std::string name;
        InputActionType type = InputActionType::BUTTON;
        float pressPoint = 0.5f;
        float holdTime = 0.0f;              // 0: a plain press; else performed after holding this long (s)
        std::optional<float> deadzone;      // AXIS, VECTOR2; else Project Settings > Input's
        std::vector<InputBindingDesc> bindings;
        InputJson passthrough = InputJson::object();
    };

    struct JASPERENGINE_API InputActionMapDesc {
        std::string name;
        int priority = 0;                   // higher first
        bool enabled = true;                // at load
        bool consume = true;                // a control it uses is unavailable to lower maps
        bool exclusive = false;             // enabled, it blocks every lower map
        bool ignoreWhenUIConsumes = true;   // a press the UI took does not count
        std::vector<InputActionDesc> actions;
        InputJson passthrough = InputJson::object();
    };

    struct JASPERENGINE_API InputActionAssetDesc {
        static constexpr int FORMAT_VERSION = 1;

        int version = FORMAT_VERSION;
        std::vector<InputActionMapDesc> maps;
        InputJson passthrough = InputJson::object();

        // false + error for text that is not an asset; a binding without an id gets one
        static bool parse(const std::string& text, InputActionAssetDesc& out, std::string& error);
        static bool fromJson(const InputJson& json, InputActionAssetDesc& out, std::string& error);
        InputJson toJson() const;
        // the file's text (2-space indent, a final newline)
        std::string save() const;

        InputActionMapDesc* findMap(const std::string& name);
        const InputActionMapDesc* findMap(const std::string& name) const;
        // "Map/Action"
        InputActionDesc* findAction(const std::string& mapSlashAction);
        const InputActionDesc* findAction(const std::string& mapSlashAction) const;
        // a binding or composite part by id
        InputBindingDesc* findBinding(const std::string& id);
        const InputBindingDesc* findBinding(const std::string& id) const;

        // a fresh id no binding of the asset has
        std::string newBindingId() const;
        // gives every binding without an id (or with one another binding has) a fresh one
        void assignMissingIds();
    };

    // modifier names <-> D_MODIFIER_* bits: "shift", "ctrl", "alt", "super"
    JASPERENGINE_API int inputModifiersFromNames(const std::vector<std::string>& names);
    JASPERENGINE_API std::vector<std::string> inputModifierNames(int mods);

    JASPERENGINE_API const char* inputActionTypeName(InputActionType type);
    JASPERENGINE_API bool inputActionTypeFromName(const std::string& name, InputActionType& type);
    JASPERENGINE_API const char* inputCompositeName(InputCompositeType type);
    // the part names of a composite, in order
    JASPERENGINE_API std::vector<std::string> inputCompositeParts(InputCompositeType type);

    // ---- edit operations (the editor's commands and edit_input_actions)
    //
    // An op is JSON: {op, ...}; apply throws std::invalid_argument with the reason when it
    // cannot apply (nothing is changed then). Addresses: map "Map", action "Map/Action",
    // binding by id.
    //   add_map {name, priority?, enabled?, consume?, exclusive?, ignore_when_ui_consumes?, index?}
    //   add_action {map, name, type?, press_point?, hold_time?, deadzone?, index?}
    //   add_binding {action, path, scale?, invert?, deadzone?, required_mods?, forbidden_mods?, groups?, index?}
    //   add_composite {action, composite: axis1d|dpad2d, parts: {part: path | {path, ...}}, groups?, normalize?, index?}
    //   set {map | action | binding, values: {key: value}}   (the asset's JSON keys)
    //   remove {map | action | binding}
    //   rename {map | action, name}
    //   move {map | action | binding, index}                 (within its parent)
    //   duplicate {map | action | binding}                   (fresh ids)
    // Returns a human name for the undo step ("Add Action Gameplay/Jump").
    JASPERENGINE_API std::string applyInputActionsOp(InputActionAssetDesc& asset, const InputJson& op);

}

#endif // INPUTACTIONASSET_H
