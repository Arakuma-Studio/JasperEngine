// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (anim layers spec 11.1)
// - Purpose: the Animator Controller asset (.controller, Unity's AnimatorController): parameters
//   and, per layer, a state machine (states with a clip or a 1D blend tree, an Any State,
//   ordered transitions with conditions, exit time, duration, offset and interruption). An
//   Animation Player with `controller` set plays it (the driver: ActionSystem::driveController).
//   * ControllerData: the value form, parsed and written by parseController / writeController
//     (AnimatorControllerPool, the editor's controller document, MCP edit_animator, the CLI).
//   * AnimatorController: the immutable runtime form (AnimatorControllerPool): the data with its
//     names resolved to indices.
// - Format: UTF-8 JSON {"format": "jasperengine.controller", "version": 1, "guid", "parameters",
//   "layers", unknown keys...}. Canonical and lenient like .anim: format + version required
//   (Unity YAML refused), every other bad value dropped or repaired with a warning carrying its
//   JSON path, unknown keys (top level, layers, states, transitions, blend trees and their
//   children) kept and written back. writeController(parseController(writeController(d))) ==
//   writeController(d) byte for byte.
// - Canonical details: 2-space indent; "guid" only when set; one parameter, state and transition
//   per line; a layer is a block. Parameters {"name", "type", "default"} ("default" not for
//   triggers). Layers {"name", "weight" (not 1), "blend" (not override), "mask"/"mask_guid" (when
//   set), "default_state" (when set), "any_state" (when it has a position), "states",
//   "transitions"}. States {"name", "clip"/"clip_guid" or "blend_tree", "speed" (not 1),
//   "speed_parameter", "wrap" (not default), "cycle_offset" (not 0), "mirror"/"foot_ik"/"retarget"
//   (when true), "position" (when set)}. Transitions {"from", "to", "conditions" (when any),
//   "has_exit_time", "exit_time", "duration", "fixed_duration", "offset", "interruption" (not
//   none), "can_transition_to_self" (Any State transitions only)}. Numbers %.6g.
// - Invariants of parsed / normalized data: parameter names unique and non-empty; per layer,
//   state names unique, non-empty and not "any"; transitions name existing states ("any" only
//   as `from`); a layer's default_state names one of its states (or is empty with no states);
//   blend tree children sorted by threshold (automate_thresholds: spaced evenly over [0, 1]);
//   layer names unique, at least one layer ("Base Layer" when the file has none).
// - Thread-safety: pure functions; an AnimatorController never changes after build.

#ifndef ANIMATOR_CONTROLLER_H
#define ANIMATOR_CONTROLLER_H

#include "Export.h"
#include "animation/AnimationClipIO.h"
#include "animation/ClipTypes.h"
#include "component/AnimationPlayerComponent.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::anim{

    constexpr const char* CONTROLLER_FORMAT = "jasperengine.controller";
    constexpr int CONTROLLER_VERSION = 1;
    constexpr const char* CONTROLLER_EXTENSION = ".controller";
    // the reserved `from` of Any State transitions
    constexpr const char* ANY_STATE = "any";

    using KeptKeys = std::vector<std::pair<std::string, std::string>>;

    enum class ControllerParameterType : uint8_t { Float, Int, Bool, Trigger };
    // "float", "int", "bool", "trigger"
    JASPERENGINE_API const char* parameterTypeName(ControllerParameterType type);
    JASPERENGINE_API bool parameterTypeFromName(const std::string& name, ControllerParameterType& out);

    struct JASPERENGINE_API ControllerParameter{
        std::string name;
        ControllerParameterType type = ControllerParameterType::Float;
        // float: the value; int: a whole number; bool: 0 or 1; trigger: 0
        float defaultValue = 0.0f;
    };

    // Unity's AnimatorConditionMode
    enum class ConditionMode : uint8_t { If, IfNot, Greater, Less, Equals, NotEqual };
    // "if", "if_not", "greater", "less", "equals", "not_equal"
    JASPERENGINE_API const char* conditionModeName(ConditionMode mode);
    JASPERENGINE_API bool conditionModeFromName(const std::string& name, ConditionMode& out);
    // the modes a parameter type takes (greater/less: float, int; equals/not_equal: int;
    // if/if_not: bool; trigger: if)
    JASPERENGINE_API bool conditionModeFits(ConditionMode mode, ControllerParameterType type);

    struct JASPERENGINE_API ControllerCondition{
        std::string parameter;
        ConditionMode mode = ConditionMode::If;
        float threshold = 0.0f;         // greater, less, equals, not_equal
    };

    enum class TransitionInterruption : uint8_t { None, CurrentState, NextState };
    // "none", "current_state", "next_state"
    JASPERENGINE_API const char* interruptionName(TransitionInterruption interruption);
    JASPERENGINE_API bool interruptionFromName(const std::string& name, TransitionInterruption& out);

    struct JASPERENGINE_API ControllerTransition{
        std::string from;               // a state name or "any"
        std::string to;
        std::vector<ControllerCondition> conditions;
        // Unity's defaults for new transitions
        bool hasExitTime = true;
        float exitTime = 0.75f;         // normalized time of the source
        float duration = 0.25f;
        bool fixedDuration = true;      // seconds; false: a fraction of the source's length
        float offset = 0.0f;            // normalized start of the destination
        TransitionInterruption interruption = TransitionInterruption::None;
        bool canTransitionToSelf = true;    // Any State transitions only
        KeptKeys unknownKeys;
    };

    struct JASPERENGINE_API BlendTreeChild{
        std::string clip;               // asset-relative .anim or "<model>#<clip>"
        std::string clipGuid;           // .anim: its GUID (editor)
        float threshold = 0.0f;
        float speed = 1.0f;
        bool mirror = false;            // anim-retarget
        KeptKeys unknownKeys;
    };

    // A 1D blend tree (Unity BlendTree, Blend Type 1D)
    struct JASPERENGINE_API BlendTree1D{
        std::string parameter;          // a float or int parameter
        std::vector<BlendTreeChild> children;
        bool automateThresholds = false;
        KeptKeys unknownKeys;
    };

    struct JASPERENGINE_API ControllerState{
        std::string name;
        // the motion: a clip, a blend tree, or neither (an empty state: writes nothing)
        std::string clip;
        std::string clipGuid;
        bool hasBlendTree = false;
        BlendTree1D blendTree;
        float speed = 1.0f;
        std::string speedParameter;     // a float parameter multiplied in
        ClipWrapMode wrap = ClipWrapMode::Default;  // Default: the clip's own
        float cycleOffset = 0.0f;       // 0..1, the normalized start
        // consumed by anim-retarget (kept and ignored before it)
        bool mirror = false;
        bool footIK = false;
        bool retarget = false;
        bool hasPosition = false;       // editor only: the graph position
        float position[2] = {0.0f, 0.0f};
        KeptKeys unknownKeys;

        bool isEmpty() const { return clip.empty() && !hasBlendTree; }
    };

    struct JASPERENGINE_API ControllerLayer{
        std::string name;
        float weight = 1.0f;            // ignored for layer 0
        AnimationLayerBlend blend = AnimationLayerBlend::Override;
        std::string mask;
        std::string maskGuid;
        std::string defaultState;       // "" = the first state
        bool hasAnyStatePosition = false;
        float anyStatePosition[2] = {0.0f, 0.0f};
        std::vector<ControllerState> states;
        std::vector<ControllerTransition> transitions;
        KeptKeys unknownKeys;

        int findState(const std::string& name) const;
        // the default state's index (-1 without states)
        int defaultStateIndex() const;
    };

    struct JASPERENGINE_API ControllerData{
        std::string guid;
        std::vector<ControllerParameter> parameters;
        std::vector<ControllerLayer> layers;
        KeptKeys unknownKeys;

        int findParameter(const std::string& name) const;
        int findLayer(const std::string& name) const;
    };

    struct JASPERENGINE_API ControllerParseResult{
        std::optional<ControllerData> data;
        std::vector<ClipWarning> warnings;
        ClipParseError error;

        bool ok() const { return data.has_value(); }
    };

    JASPERENGINE_API ControllerParseResult parseController(const std::string& text);
    // the canonical text of `data` (normalized first)
    JASPERENGINE_API std::string writeController(const ControllerData& data);
    // restores the invariants above on hand-built data; what changed goes to `warnings`
    JASPERENGINE_API void normalizeController(ControllerData& data, std::vector<ClipWarning>* warnings = nullptr);

    // Unity's YAML controller (%YAML header or an "AnimatorController:" document)
    JASPERENGINE_API bool isUnityYamlController(const std::string& text);
    JASPERENGINE_API const char* unityYamlControllerRefusal();

    // What validation knows of a clip: whether it loads, and whether it loops
    struct JASPERENGINE_API ControllerClipInfo{
        bool loads = false;
        bool loops = false;             // its wrap (or the state's) is loop or ping_pong
        std::string error;
    };
    using ControllerClipResolver = std::function<ControllerClipInfo(const std::string& clip)>;

    // The spec 5.11 checks (JSON paths into the file): unknown clips (with `resolve`; null skips
    // them), parameters missing or of the wrong type, dead-end states (no way out, and the
    // motion does not loop), states no transition reaches, transitions with neither conditions
    // nor exit time (they fire at once), parameters nothing reads ("unused")
    JASPERENGINE_API std::vector<ClipWarning> validateController(const ControllerData& data, const ControllerClipResolver& resolve);

    // The runtime form: the normalized data with names resolved to indices (-1: none)
    class JASPERENGINE_API AnimatorController{
    public:
        struct Condition{
            int parameter = -1;
            ConditionMode mode = ConditionMode::If;
            float threshold = 0.0f;
        };
        struct Transition{
            int from = -1;              // -1: Any State
            int to = -1;
            std::vector<Condition> conditions;
            const ControllerTransition* data = nullptr;
        };
        struct State{
            const ControllerState* data = nullptr;
            int speedParameter = -1;
            int blendParameter = -1;
            std::vector<int> transitions;   // indices into Layer::transitions from this state, in order
        };
        struct Layer{
            const ControllerLayer* data = nullptr;
            int defaultState = -1;
            std::vector<State> states;
            std::vector<Transition> transitions;
            std::vector<int> anyTransitions;    // Any State transitions, in order
        };

        AnimatorController(const AnimatorController&) = delete;
        AnimatorController& operator=(const AnimatorController&) = delete;

        static std::shared_ptr<const AnimatorController> build(const ControllerData& data, std::vector<ClipWarning> warnings = {});

        const ControllerData& getData() const { return data; }
        const std::vector<Layer>& getLayers() const { return layers; }
        const std::vector<ClipWarning>& getWarnings() const { return warnings; }

    private:
        AnimatorController() = default;
        ControllerData data;
        std::vector<Layer> layers;
        std::vector<ClipWarning> warnings;
    };

}

#endif // ANIMATOR_CONTROLLER_H
