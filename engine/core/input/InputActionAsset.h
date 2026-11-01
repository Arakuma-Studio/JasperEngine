// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef INPUTACTIONASSET_H
#define INPUTACTIONASSET_H

// The .inputactions asset (docs/specs/S22-gameplay.md R8, R44): JSON with "version": 1 holding
// action maps -> actions -> bindings. The data model, its JSON form and the edit operations of
// the Input Actions window and the MCP edit_input_actions tool. Unknown keys at every level are
// written back as they were, and the output is deterministic (the same model, the same bytes).
// No JSON library in this header: it is part of the scripts' engine API (JasperEngine.h). The JSON
// form and the edit operations are in input/InputActionAssetJson.h.

#include "Export.h"

#include <optional>
#include <string>
#include <vector>

namespace jasperengine {

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

    // Unity's Input Manager smoothing of a binding (its "Key or Mouse Button" entries): the value
    // moves to the binding's target at sensitivity/s and back to 0 at gravity/s (<= 0: at once),
    // a value under dead reads 0, snap jumps through 0 when the target reverses. Runs on the
    // unscaled frame time (an action works at time scale 0)
    struct JASPERENGINE_API InputSmoothing {
        float gravity = 3.0f;
        float sensitivity = 3.0f;
        float dead = 0.001f;
        bool snap = false;

        bool operator==(const InputSmoothing& other) const{
            return gravity == other.gravity && sensitivity == other.sensitivity && dead == other.dead && snap == other.snap;
        }
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
        // the gamepad its gamepad controls read (0..15), -1: the map's (InputActions::setGamepad)
        int gamepad = -1;
        // Input Manager smoothing of its value (none: the value as the control gives it)
        std::optional<InputSmoothing> smoothing;
        InputCompositeType composite = InputCompositeType::NONE;
        bool normalize = true;              // DPAD_2D: the vector's length is at most 1
        // a composite's parts by role (negative/positive, up/down/left/right), in role order; a
        // role may have several parts (any of them actuates it: the larger magnitude counts) or
        // none (it reads 0), as an Input Manager entry's positive and negative button lists
        std::vector<std::pair<std::string, InputBindingDesc>> parts;
        std::string passthrough;            // the unknown keys: a compact JSON object, empty for none

        bool isComposite() const { return composite != InputCompositeType::NONE; }
        bool inGroup(const std::string& group) const;
    };

    struct JASPERENGINE_API InputActionDesc {
        std::string name;                   // unique in its map; may hold '/' (an Input Manager name)
        std::string description;
        InputActionType type = InputActionType::BUTTON;
        float pressPoint = 0.5f;
        float holdTime = 0.0f;              // 0: a plain press; else performed after holding this long (s)
        std::optional<float> deadzone;      // AXIS, VECTOR2; else Project Settings > Input's
        std::vector<InputBindingDesc> bindings;
        std::string passthrough;            // the unknown keys: a compact JSON object, empty for none
    };

    struct JASPERENGINE_API InputActionMapDesc {
        std::string name;
        int priority = 0;                   // higher first
        bool enabled = true;                // at load
        bool consume = true;                // a control it uses is unavailable to lower maps
        bool exclusive = false;             // enabled, it blocks every lower map
        bool ignoreWhenUIConsumes = true;   // a press the UI took does not count
        std::vector<InputActionDesc> actions;
        std::string passthrough;            // the unknown keys: a compact JSON object, empty for none
    };

    struct JASPERENGINE_API InputActionAssetDesc {
        static constexpr int FORMAT_VERSION = 1;

        int version = FORMAT_VERSION;
        std::vector<InputActionMapDesc> maps;
        std::string passthrough;            // the unknown keys: a compact JSON object, empty for none

        // false + error for text that is not an asset; a binding without an id gets one
        static bool parse(const std::string& text, InputActionAssetDesc& out, std::string& error);
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
    // the part names (roles) of a composite, in order
    JASPERENGINE_API std::vector<std::string> inputCompositeParts(InputCompositeType type);
    // a role's index in that order, -1 for none
    JASPERENGINE_API int inputCompositePartIndex(InputCompositeType type, const std::string& part);


}

#endif // INPUTACTIONASSET_H
