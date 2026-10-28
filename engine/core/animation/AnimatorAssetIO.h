// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S21-animation.md R32, D13, D18)
// - Purpose: the readers and canonical writers of `.controller`, `.mask` and `.montage` files,
//   their normalization, and the enum <-> JSON name tables the files, Lua and MCP share.
// - Format: UTF-8 JSON objects with "format" ("jasperengine.controller" / ".mask" / ".montage")
//   and "version" (1), then "guid" (32 lowercase hex, as .anim), then the asset's members.
//   Every reader is lenient like AnimationClipIO: only a missing/different format, a newer
//   version, or text that is not a JSON object fails (with line:col); a bad member is dropped
//   with a warning carrying its JSON path ("layers[0].state_machine.states[2].speed: expected a
//   number"), unknown top-level keys are kept and written back.
// - Canonical text: fixed member order, 2-space indent, one member per line, arrays of numbers on
//   one line, numbers with %.7g (integers plain). write(parse(write(d))) == write(d), and
//   parse + write of a canonical file is a no-op. Members equal to their defaults are written
//   too, except the optional ones listed per type below (absent = default).
// - Migrations: `migrate*` take the parsed JSON of an older version to the current one before
//   it is read (version 1 is the only version today: the tables are empty, a newer version is
//   refused with "made by a newer JasperEngine").
// - Thread-safety: pure functions.

#ifndef ANIMATOR_ASSET_IO_H
#define ANIMATOR_ASSET_IO_H

#include "Export.h"
#include "action/Ease.h"
#include "animation/ClipTypes.h"
#include "animation/ControllerDef.h"

#include <string>
#include <vector>

namespace jasperengine::anim{

    constexpr const char* CONTROLLER_FORMAT = "jasperengine.controller";
    constexpr const char* MASK_FORMAT = "jasperengine.mask";
    constexpr const char* MONTAGE_FORMAT = "jasperengine.montage";
    constexpr int ANIMATOR_ASSET_VERSION = 1;
    constexpr const char* CONTROLLER_EXTENSION = ".controller";
    constexpr const char* MASK_EXTENSION = ".mask";
    constexpr const char* MONTAGE_EXTENSION = ".montage";
    // the most children of one blend tree (R6) and sections / events of one montage
    constexpr size_t MAX_BLEND_CHILDREN = 32;
    constexpr size_t MAX_MONTAGE_SECTIONS = 64;
    constexpr size_t MAX_MONTAGE_EVENTS = 256;
    // .anim top-level members of this stream that AnimationClipIO keeps as unknown keys without a
    // warning ("root_motion", "editor")
    JASPERENGINE_API bool isClipExtensionKey(const std::string& key);

    // A failed parse: where (1-based line:col, 0 when not at a text position) and why
    struct JASPERENGINE_API AnimatorParseError{
        int line = 0;
        int column = 0;
        std::string message;

        std::string toString() const;
    };

    template<class T>
    struct AnimatorParseResult{
        bool ok = false;
        AnimatorParseError error;
        T data;
        std::vector<ClipWarning> warnings;
    };

    using ControllerParseResult = AnimatorParseResult<ControllerDef>;
    using MaskParseResult = AnimatorParseResult<AvatarMaskDef>;
    using MontageParseResult = AnimatorParseResult<MontageDef>;

    // Controller members: format, version, guid?, parameters, layers. A parameter is {name, type,
    // default?}; a layer {name, weight, blending, mask?, slot?, state_machine}; a machine {name?
    // (sub-machines), position?, default_state, entry, any_state, states, sub_state_machines,
    // exit_transitions?, entry_position, any_state_position, exit_position}; a state {name,
    // position, motion, speed, speed_parameter?, cycle_offset?, cycle_offset_parameter?,
    // time_parameter?, loop?, tag?, additive_reference?, additive_time?, transitions}; a motion
    // null, {"clip": ref} or {"blend_tree": {type, parameter, parameter_y?, children}}; a child
    // {motion, threshold | position, time_scale?, direct_parameter?}; a transition {to,
    // conditions, has_exit_time, exit_time, fixed_duration, duration, offset,
    // interruption_source, ordered_interruption, can_transition_to_self?, mute?}; a condition
    // {parameter, mode, threshold?}; a ref {path, guid?}.
    JASPERENGINE_API ControllerParseResult parseController(const std::string& text);
    JASPERENGINE_API std::string writeController(const ControllerDef& data);
    // unique names (a later duplicate gets " 1", " 2"... with a warning), clamps (weights 0..1,
    // durations >= 0, at most MAX_BLEND_CHILDREN children), the first layer weight 1
    JASPERENGINE_API void normalizeController(ControllerDef& data, std::vector<ClipWarning>* warnings = nullptr);

    // Mask members: format, version, guid?, body_parts {root, body, head, left_arm, right_arm,
    // left_hand, right_hand, left_leg, right_leg: bool}, paths [{path, weight}]
    JASPERENGINE_API MaskParseResult parseMask(const std::string& text);
    JASPERENGINE_API std::string writeMask(const AvatarMaskDef& data);
    // paths unique (the later wins) and sorted, weights clamped to 0..1
    JASPERENGINE_API void normalizeMask(AvatarMaskDef& data, std::vector<ClipWarning>* warnings = nullptr);

    // Montage members: format, version, guid?, clip, slot, rate, blend_in {time, ease}, blend_out,
    // sections [{name, start, next}], events [{time, function, args?}]
    JASPERENGINE_API MontageParseResult parseMontage(const std::string& text);
    JASPERENGINE_API std::string writeMontage(const MontageDef& data);
    // sections sorted by start, unique names, the first at 0 ("Default" when there is none),
    // unknown `next` names cleared with a warning; events sorted by time
    JASPERENGINE_API void normalizeMontage(MontageDef& data, std::vector<ClipWarning>* warnings = nullptr);

    // "controller", "mask", "montage" or "" from a file's "format" member (cheap scan)
    JASPERENGINE_API std::string animatorAssetKind(const std::string& text);
    // the "guid" member of any of the three, "" when there is none (cheap scan)
    JASPERENGINE_API std::string readAnimatorAssetGuid(const std::string& text);

    // ---- names (JSON, Lua, MCP) ----

    JASPERENGINE_API const char* parameterTypeName(AnimatorParameterType type);
    JASPERENGINE_API bool parameterTypeFromName(const std::string& name, AnimatorParameterType& out);
    JASPERENGINE_API const char* conditionModeName(AnimatorConditionMode mode);
    JASPERENGINE_API bool conditionModeFromName(const std::string& name, AnimatorConditionMode& out);
    JASPERENGINE_API const char* layerBlendingName(AnimatorLayerBlending blending);
    JASPERENGINE_API bool layerBlendingFromName(const std::string& name, AnimatorLayerBlending& out);
    JASPERENGINE_API const char* blendTreeTypeName(BlendTreeType type);
    JASPERENGINE_API bool blendTreeTypeFromName(const std::string& name, BlendTreeType& out);
    JASPERENGINE_API const char* interruptionSourceName(TransitionInterruptionSource source);
    JASPERENGINE_API bool interruptionSourceFromName(const std::string& name, TransitionInterruptionSource& out);
    JASPERENGINE_API const char* stateLoopName(AnimatorStateLoop loop);
    JASPERENGINE_API bool stateLoopFromName(const std::string& name, AnimatorStateLoop& out);
    JASPERENGINE_API const char* additiveReferenceName(AdditiveReference reference);
    JASPERENGINE_API bool additiveReferenceFromName(const std::string& name, AdditiveReference& out);
    JASPERENGINE_API const char* bodyPartName(AvatarBodyPart part);
    JASPERENGINE_API bool bodyPartFromName(const std::string& name, AvatarBodyPart& out);
    JASPERENGINE_API const char* ikTypeName(AnimatorIKType type);
    JASPERENGINE_API bool ikTypeFromName(const std::string& name, AnimatorIKType& out);

    // ---- .anim root motion (an extension member kept by AnimationClipIO) ----

    struct JASPERENGINE_API ClipRootMotion{
        bool present = false;           // the clip declares a root_motion block
        std::string bone;               // path; empty: the Animator's choice (R10)
        bool bakeRotation = false;      // keep the heading in the pose (no turning root motion)
        bool bakePositionY = true;      // keep the height in the pose
        bool bakePositionXZ = false;    // keep the horizontal motion in the pose
    };

    // from a clip's unknown keys ("root_motion"); warnings for bad members
    JASPERENGINE_API ClipRootMotion readClipRootMotion(const std::vector<std::pair<std::string, std::string>>& unknownKeys,
                                                       std::vector<ClipWarning>* warnings = nullptr);
    // the compact JSON of a root_motion block (for ClipData::unknownKeys)
    JASPERENGINE_API std::string writeClipRootMotion(const ClipRootMotion& rootMotion);

}

#endif //ANIMATOR_ASSET_IO_H
