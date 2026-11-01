// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S21-animation.md "Upstream absorption")
// - Purpose: the readers of upstream's (origin/main anim-layers) version 1 `.controller` and
//   `.mask` files, and their conversion to S21's ControllerDef / AvatarMaskDef. Both dialects
//   wrote "format": "jasperengine.controller" / "jasperengine.mask" with "version": 1, so the
//   header cannot tell them apart: AnimatorAssetIO picks the dialect by the members a version 1
//   file has and calls these for upstream's. Nothing writes upstream's dialect any more: a
//   converted file is saved as S21's version 2.
// - Upstream's controller: {"format", "version": 1, "guid", "parameters": [{name, type,
//   default}], "layers": [{name, weight, blend, mask, mask_guid, default_state, any_state:
//   {position}, states: [{name, clip, clip_guid, blend_tree: {parameter, children: [{clip,
//   clip_guid, threshold, speed, mirror}], automate_thresholds}, speed, speed_parameter, wrap,
//   cycle_offset, mirror, foot_ik, retarget, position}], transitions: [{from ("any": Any
//   State), to, conditions, has_exit_time (default true), exit_time, duration, fixed_duration,
//   offset, interruption (none / current_state / next_state), can_transition_to_self (default
//   true)}]}]}; unknown members are kept at every level.
// - Upstream's mask: {"format", "version": 1, "guid", "default" (1), "humanoid": {root, body,
//   head, left_leg, right_leg, left_arm, right_arm, left_fingers, right_fingers: 0..1},
//   "transforms": [{path, weight, children (default true)}], "preview": {model}}.
// - Conversion (lossless: every member lands in an S21 member or in the unknown keys of the
//   matching S21 object, where a save writes it back):
//   * controller: flat states and transitions nest into each layer's state_machine (from
//     "any" -> any_state); "blend" -> blending; string refs + *_guid -> {path, guid};
//     "interruption" -> interruption_source (current_state -> current, next_state -> next,
//     ordered); upstream's defaults made explicit (has_exit_time true, Any State
//     can_transition_to_self true); "wrap" -> "loop" (default -> inherit, loop -> repeat,
//     ping_pong, once, clamp_forever); additive references -> "clip" (the .anim's own
//     additive_reference, upstream's rule); "speed" of a child -> time_scale; a state's "mirror",
//     "foot_ik", "retarget" and a child's "mirror" -> S21's state / child fields of the same
//     names (anim-retarget's humanoid playback, on the one retarget path); "automate_thresholds"
//     -> an unknown key of the tree; graph positions kept (a state without one
//     gets a grid place). Names S21 reserves ("Entry", "Exit", "Any State") or with '/' or '.'
//     are renamed by S21's normalization with a warning.
//   * mask: "default" -> default; "humanoid" parts -> body_parts (<side>_arm also weighs the
//     hand bone, <side>_fingers what is below it: upstream's hand belonged to the arm);
//     "transforms" -> paths (children kept); "preview.model" -> preview; root_hierarchy true
//     (upstream's root part covered the root and the transforms above the hips).
// - Thread-safety: pure functions.

#ifndef UPSTREAM_ANIM_FORMATS_H
#define UPSTREAM_ANIM_FORMATS_H

#include "Export.h"
#include "animation/AnimationClipIO.h"
#include "animation/ClipTypes.h"
#include "animation/ControllerDef.h"
#include "component/AnimationPlayerComponent.h"

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::anim::upstream{

    using KeptKeys = std::vector<std::pair<std::string, std::string>>;

    // the reserved `from` of Any State transitions
    constexpr const char* ANY_STATE = "any";

    enum class ParameterType : uint8_t { Float, Int, Bool, Trigger };
    enum class ConditionMode : uint8_t { If, IfNot, Greater, Less, Equals, NotEqual };
    enum class Interruption : uint8_t { None, CurrentState, NextState };

    struct JASPERENGINE_API Parameter{
        std::string name;
        ParameterType type = ParameterType::Float;
        float defaultValue = 0.0f;
    };

    struct JASPERENGINE_API Condition{
        std::string parameter;
        ConditionMode mode = ConditionMode::If;
        float threshold = 0.0f;
    };

    struct JASPERENGINE_API Transition{
        std::string from;
        std::string to;
        std::vector<Condition> conditions;
        bool hasExitTime = true;
        float exitTime = 0.75f;
        float duration = 0.25f;
        bool fixedDuration = true;
        float offset = 0.0f;
        Interruption interruption = Interruption::None;
        bool canTransitionToSelf = true;
        KeptKeys unknownKeys;
    };

    struct JASPERENGINE_API TreeChild{
        std::string clip;
        std::string clipGuid;
        float threshold = 0.0f;
        float speed = 1.0f;
        bool mirror = false;
        KeptKeys unknownKeys;
    };

    struct JASPERENGINE_API Tree{
        std::string parameter;
        std::vector<TreeChild> children;
        bool automateThresholds = false;
        KeptKeys unknownKeys;
    };

    struct JASPERENGINE_API State{
        std::string name;
        std::string clip;
        std::string clipGuid;
        bool hasBlendTree = false;
        Tree blendTree;
        float speed = 1.0f;
        std::string speedParameter;
        ClipWrapMode wrap = ClipWrapMode::Default;
        float cycleOffset = 0.0f;
        bool mirror = false;
        bool footIK = false;
        bool retarget = false;
        bool hasPosition = false;
        float position[2] = {0.0f, 0.0f};
        KeptKeys unknownKeys;
    };

    struct JASPERENGINE_API Layer{
        std::string name;
        float weight = 1.0f;
        AnimationLayerBlend blend = AnimationLayerBlend::Override;
        std::string mask;
        std::string maskGuid;
        std::string defaultState;
        bool hasAnyStatePosition = false;
        float anyStatePosition[2] = {0.0f, 0.0f};
        std::vector<State> states;
        std::vector<Transition> transitions;
        KeptKeys unknownKeys;

        int findState(const std::string& name) const;
    };

    struct JASPERENGINE_API Controller{
        std::string guid;
        std::vector<Parameter> parameters;
        std::vector<Layer> layers;
        KeptKeys unknownKeys;
    };

    // Unity's AvatarMaskBodyPart without the IK goals, upstream's order and names
    enum class MaskPart : uint8_t { Root, Body, Head, LeftLeg, RightLeg, LeftArm, RightArm, LeftFingers, RightFingers, Count };
    constexpr int MASK_PART_COUNT = (int)MaskPart::Count;

    struct JASPERENGINE_API MaskTransform{
        std::string path;
        float weight = 1.0f;
        bool children = true;
    };

    struct JASPERENGINE_API Mask{
        std::string guid;
        float defaultWeight = 1.0f;
        bool hasHumanoid = false;
        std::array<std::optional<float>, MASK_PART_COUNT> parts;
        std::vector<MaskTransform> transforms;
        std::string previewModel;
        KeptKeys unknownKeys;
    };

    template<class T>
    struct ParseResult{
        std::optional<T> data;
        std::vector<ClipWarning> warnings;
        ClipParseError error;

        bool ok() const { return data.has_value(); }
    };

    // upstream's readers (lenient: warnings with JSON paths, upstream's normalization applied:
    // parameters Lua-safe and unique, states unique and not "any", transitions between existing
    // states, weights clamped, blend tree children sorted or spaced by automate_thresholds)
    JASPERENGINE_API ParseResult<Controller> parseController(const std::string& text);
    JASPERENGINE_API ParseResult<Mask> parseMask(const std::string& text);

    // the S21 forms (header comment); conversion warnings (what S21's runtime reads otherwise)
    // go to `warnings`
    JASPERENGINE_API ControllerDef toControllerDef(const Controller& data, std::vector<ClipWarning>* warnings = nullptr);
    JASPERENGINE_API AvatarMaskDef toMaskDef(const Mask& data);

    JASPERENGINE_API const char* maskPartName(MaskPart part);

}

#endif // UPSTREAM_ANIM_FORMATS_H
