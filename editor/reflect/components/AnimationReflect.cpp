// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Actions, tweens, animations, bones and keyframe tracks (docs/specs/S08-reflection.md P2/P3).
// Field order is the YAML key order.

#include "reflect/Components.h"

#include "Factory.h"
#include "Stream.h"
#include "component/ActionComponent.h"
#include "component/AlphaTweenComponent.h"
#include "component/AnimationComponent.h"
#include "component/BoneComponent.h"
#include "component/ColorTweenComponent.h"
#include "component/KeyframeTracksComponent.h"
#include "component/MorphTracksComponent.h"
#include "component/PositionTweenComponent.h"
#include "component/RotateTracksComponent.h"
#include "component/RotationTweenComponent.h"
#include "component/ScaleTracksComponent.h"
#include "component/ScaleTweenComponent.h"
#include "component/SpriteAnimationComponent.h"
#include "component/TranslateTracksComponent.h"
#include "component/TweenComponent.h"

namespace jasperengine::editor::reflect {

namespace {

    // ---------------------------------------------------------------- Action

    // an exported action starts as it was saved: running or paused ones through their triggers
    void actionStateStatements(const FieldDesc&, const void* value, const std::string&, const CodegenContext& ctx, std::string& out) {
        const ActionState state = *static_cast<const ActionState*>(value);
        const std::string ind = Factory::indentation(ctx.indentSpaces);
        if (state == ActionState::Running) {
            out += ind + ctx.variable + ".startTrigger = true;\n";
        } else if (state == ActionState::Paused) {
            out += ind + ctx.variable + ".startTrigger = true;\n";
            out += ind + ctx.variable + ".pauseTrigger = true;\n";
        } else {
            out += ind + ctx.variable + ".state = " + Factory::formatActionState(state) + ";\n";
        }
    }

    constexpr TypeOps kActionStateOps = detail::withStatements(ops::Enum<ActionState>, &actionStateStatements);

    constexpr FieldDesc kActionFields[] = {
        JE_FIELD(ActionComponent, state, .ops = &kActionStateOps, .enumDesc = &enums::kActionState, .flags = NoDefault),
        JE_FIELD(ActionComponent, speed),
        // runtime-core spec 6.7: where each new run starts, in Play mode and builds
        JE_FIELD(ActionComponent, startOffset, .flags = OmitIfDefault | CodegenIfChanged, .unit = Unit::Seconds, .min = 0.0f,
                 .tooltip = "Seconds into the action each new run starts at, in Play mode and builds (previews start at 0)"),
        JE_FIELD(ActionComponent, randomStart, .flags = OmitIfDefault | CodegenIfChanged,
                 .tooltip = "Each new run starts at a random point of one cycle (different per GameObject, the same every "
                            "playthrough); overrides Start Offset"),
        JE_FIELD(ActionComponent, target, .ops = &ops::Entity),
        JE_FIELD(ActionComponent, ownedTarget),
    };

    // a decoded action is not running: its timer and triggers start afresh
    void actionPostDecode(void* comp, const DecodeContext&) {
        ActionComponent& action = *static_cast<ActionComponent*>(comp);
        action.timecount = 0;
        action.phase = 0;
        action.runs = 0;
        action.startTrigger = false;
        action.stopTrigger = false;
        action.pauseTrigger = false;
    }

    // ---------------------------------------------------------------- Tweens

    bool tweenEasesAlongCurve(const void* owner) {
        return static_cast<const TweenComponent*>(owner)->function.getType() == EaseType::CURVE;
    }

    constexpr FieldDesc kTweenFields[] = {
        JE_FIELD(TweenComponent, duration, .unit = Unit::Seconds, .min = 0.0f),
        JE_FIELD(TweenComponent, loop),
        JE_FIELD(TweenComponent, pingPong, .label = "Ping-pong", .tooltip = "Plays forward and then back, in twice the duration"),
        JE_FIELD(TweenComponent, function, .label = "Ease"),
        // upstream (coroutines / Time API): advances at time scale 0
        JE_FIELD(TweenComponent, useUnscaledTime, .flags = OmitIfDefault | CodegenIfChanged),
        // the ease "Curve" (docs/specs/S27-editor-tools.md R13): value = curve(time), time 0 to 1;
        // the field's view spans values -1 to 2 (overshoot)
        JE_FIELD(TweenComponent, curve, .flags = OmitIfDefault | CodegenIfChanged, .uiMin = -1.0f, .uiMax = 2.0f,
                 .tooltip = "The ease when Ease is Curve: the tween's value over its normalized time (0 to 1)",
                 .drawer = "curve", .visibleIf = &tweenEasesAlongCurve),
    };

    void tweenPostDecode(void* comp, const DecodeContext&) {
        TweenComponent& tween = *static_cast<TweenComponent*>(comp);
        tween.time = 0;
        tween.value = 0;
    }

    constexpr FieldDesc kPositionTweenFields[] = {
        JE_FIELD(PositionTweenComponent, endPosition, .flags = NoDefault),
        JE_FIELD(PositionTweenComponent, startPosition, .flags = NoDefault),
    };

    constexpr FieldDesc kRotationTweenFields[] = {
        JE_FIELD(RotationTweenComponent, endRotation, .flags = NoDefault),
        JE_FIELD(RotationTweenComponent, startRotation, .flags = NoDefault),
        JE_FIELD(RotationTweenComponent, shortestPath),
        // turns startRotation by angle around axis instead of reaching endRotation
        JE_FIELD(RotationTweenComponent, spin),
        JE_FIELD(RotationTweenComponent, axis),
        JE_FIELD(RotationTweenComponent, angle, .unit = Unit::Degrees),
    };

    constexpr FieldDesc kScaleTweenFields[] = {
        JE_FIELD(ScaleTweenComponent, endScale, .flags = NoDefault),
        JE_FIELD(ScaleTweenComponent, startScale, .flags = NoDefault),
    };

    constexpr FieldDesc kColorTweenFields[] = {
        JE_FIELD(ColorTweenComponent, endColor, .flags = NoDefault),
        JE_FIELD(ColorTweenComponent, startColor, .flags = NoDefault),
        JE_FIELD(ColorTweenComponent, useSRGB),
    };

    constexpr FieldDesc kAlphaTweenFields[] = {
        JE_FIELD(AlphaTweenComponent, endAlpha, .flags = NoDefault),
        JE_FIELD(AlphaTweenComponent, startAlpha, .flags = NoDefault),
        // upstream (ui canvas group): what the alpha drives
        JE_FIELD(AlphaTweenComponent, alphaTarget, .enumDesc = &enums::kAlphaTweenTarget, .flags = OmitIfDefault | CodegenIfChanged),
    };

    // ---------------------------------------------------------------- Animation

    constexpr FieldDesc kActionFrameFields[] = {
        JE_FIELD(ActionFrame, track, .flags = NotListed),
        JE_FIELD(ActionFrame, startTime, .flags = NotListed, .unit = Unit::Seconds),
        JE_FIELD(ActionFrame, duration, .flags = NotListed, .unit = Unit::Seconds),
        JE_FIELD(ActionFrame, action, .ops = &ops::Entity),
    };

}

JE_REFLECT_STRUCT_TRAITS(ActionFrame, kActionFrameStruct);
const StructDesc kActionFrameStruct = JE_STRUCT_DESC(ActionFrame, "ActionFrame", kActionFrameFields);

namespace {

    void actionFramesStatements(const FieldDesc&, const void* value, const std::string& target, const CodegenContext& ctx, std::string& out) {
        const std::vector<ActionFrame>& frames = *static_cast<const std::vector<ActionFrame>*>(value);
        const std::string ind = Factory::indentation(ctx.indentSpaces);
        out += ind + target + ".clear();\n";
        for (const ActionFrame& frame : frames) {
            out += ind + target + ".push_back({" + Factory::formatFloat(frame.startTime) + ", " + Factory::formatFloat(frame.duration) + ", " +
                   Factory::formatEntity(frame.action, ctx.entityVarNames) + ", " + Factory::formatUInt(frame.track) + "});\n";
        }
    }

    constexpr TypeOps kActionFramesOps = detail::withStatements(ops::Array<ActionFrame>, &actionFramesStatements);

    constexpr FieldDesc kAnimationFields[] = {
        JE_FIELD(AnimationComponent, loop),
        JE_FIELD(AnimationComponent, duration, .unit = Unit::Seconds),
        JE_FIELD(AnimationComponent, ownedActions),
        JE_FIELD(AnimationComponent, defaultFadeTime, .unit = Unit::Seconds, .min = 0.0f),
        JE_FIELD(AnimationComponent, actions, .ops = &kActionFramesOps, .flags = NullIfEmpty),
    };

    // ---------------------------------------------------------------- Bone

    void matrixStatements(const FieldDesc&, const void* value, const std::string& target, const CodegenContext& ctx, std::string& out) {
        const Matrix4& matrix = *static_cast<const Matrix4*>(value);
        const std::string ind = Factory::indentation(ctx.indentSpaces);
        out += ind + target + " = Matrix4(\n";
        for (int row = 0; row < 4; row++) {
            out += ind + "    ";
            for (int col = 0; col < 4; col++) {
                out += Factory::formatFloat(matrix[col][row]);
                if (row < 3 || col < 3) out += ", ";
            }
            out += "\n";
        }
        out += ind + ");\n";
    }

    constexpr TypeOps kOffsetMatrixOps = detail::withStatements(ops::Matrix4, &matrixStatements);

    constexpr FieldDesc kBoneFields[] = {
        JE_FIELD(BoneComponent, model, .ops = &ops::Entity, .flags = NoDefault, .drawer = "modelEntity"),
        JE_FIELD(BoneComponent, index, .flags = NoDefault),
        JE_FIELD(BoneComponent, bindPosition),
        JE_FIELD(BoneComponent, bindRotation),
        JE_FIELD(BoneComponent, bindScale),
        // the inverse bind matrix: saved, not edited
        JE_FIELD(BoneComponent, offsetMatrix, .ops = &kOffsetMatrixOps, .flags = Derived | Hidden),
    };

    // ---------------------------------------------------------------- tracks and sprite animation (the sprite animation
    // and the morph tracks keep their own codecs: a counted array, lists of lists)

    constexpr FieldDesc kSpriteAnimationFields[] = {
        JE_FIELD(SpriteAnimationComponent, loop),
        JE_FIELD(SpriteAnimationComponent, framesTimeSize, .flags = NoDefault),
        JE_FIELD(SpriteAnimationComponent, framesTime, .flags = NotListed, .count = JE_COUNT(SpriteAnimationComponent, framesTimeSize)),
        JE_FIELD(SpriteAnimationComponent, framesSize, .flags = NoDefault),
        JE_FIELD(SpriteAnimationComponent, frames, .flags = NotListed, .count = JE_COUNT(SpriteAnimationComponent, framesSize)),
        JE_FIELD(SpriteAnimationComponent, frameIndex, .flags = NoDefault, .updateFlags = UpdateFlags_Sprite),
        JE_FIELD(SpriteAnimationComponent, frameTimeIndex, .flags = NoDefault),
        JE_FIELD(SpriteAnimationComponent, spriteFrameCount, .flags = NoDefault, .updateFlags = UpdateFlags_Sprite),
    };

    constexpr uint32_t kTrackList = ListWhole | ListElements | ListTemplate;

    // the times always written ("~" when empty); the easings only when set (imported linear
    // tracks keep none), read as empty when absent
    constexpr FieldDesc kKeyframeTracksFields[] = {
        JE_FIELD(KeyframeTracksComponent, index, .flags = NoDefault),
        JE_FIELD(KeyframeTracksComponent, interpolation),
        JE_FIELD(KeyframeTracksComponent, loop, .tooltip = "Starts over after the last key"),
        // transform tracks only: values add to the target's pose when the track starts
        JE_FIELD(KeyframeTracksComponent, relative, .tooltip = "Keys are offsets from the pose the target has when the track starts"),
        JE_FIELD(KeyframeTracksComponent, times, .flags = kTrackList | NullIfEmpty, .unit = Unit::Seconds),
        JE_FIELD(KeyframeTracksComponent, easings, .enumDesc = &enums::kEaseType, .flags = kTrackList | OmitIfDefault),
    };

    // a decoded track takes its base again when it starts
    void keyframeTracksPostDecode(void* comp, const DecodeContext&) {
        static_cast<KeyframeTracksComponent*>(comp)->hasBase = false;
    }

    // values ("~" when empty), and the Hermite tangents of a GLTF CUBICSPLINE clip (kept beside
    // them by key edits): written only when set, read as empty when absent
#define JE_TRACK_FIELDS(T) \
        JE_FIELD(T, values, .flags = kTrackList | NullIfEmpty), \
        JE_FIELD(T, inTangents, .flags = ListWhole | OmitIfDefault), \
        JE_FIELD(T, outTangents, .flags = ListWhole | OmitIfDefault)

    constexpr FieldDesc kTranslateTracksFields[] = {JE_TRACK_FIELDS(TranslateTracksComponent)};
    constexpr FieldDesc kRotateTracksFields[] = {JE_TRACK_FIELDS(RotateTracksComponent)};
    constexpr FieldDesc kScaleTracksFields[] = {JE_TRACK_FIELDS(ScaleTracksComponent)};
    constexpr FieldDesc kMorphTracksFields[] = {JE_TRACK_FIELDS(MorphTracksComponent)};

#undef JE_TRACK_FIELDS

    const ComponentCodec kSpriteAnimationCodec = JE_STREAM_CODEC(SpriteAnimationComponent, encodeSpriteAnimationComponent, decodeSpriteAnimationComponent);
    const ComponentCodec kMorphTracksCodec = JE_STREAM_CODEC(MorphTracksComponent, encodeMorphTracksComponent, decodeMorphTracksComponent);


}

const ComponentDesc kActionDesc{
    .type = ComponentType::ActionComponent, .cppName = "ActionComponent", .key = "action", .displayName = "Action", .icon = ICON_FA_PLAY,
    .body = JE_STRUCT_DESC(ActionComponent, "ActionComponent", kActionFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(ActionComponent),
    .postDecode = &actionPostDecode,
    .codegenVar = "actioncomp",
};

const ComponentDesc kTweenDesc{
    .type = ComponentType::TweenComponent, .cppName = "TweenComponent", .key = "tween", .displayName = "Tween", .icon = ICON_FA_CLOCK,
    .body = JE_STRUCT_DESC(TweenComponent, "TweenComponent", kTweenFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(TweenComponent),
    .postDecode = &tweenPostDecode,
    .codegenVar = "timedcomp",
};

const ComponentDesc kPositionTweenDesc{
    .type = ComponentType::PositionTweenComponent, .cppName = "PositionTweenComponent", .key = "positiontween", .displayName = "Position Tween",
    .icon = ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT,
    .body = JE_STRUCT_DESC(PositionTweenComponent, "PositionTweenComponent", kPositionTweenFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(PositionTweenComponent),
    .codegenVar = "poscomp",
};

const ComponentDesc kRotationTweenDesc{
    .type = ComponentType::RotationTweenComponent, .cppName = "RotationTweenComponent", .key = "rotationtween", .displayName = "Rotation Tween",
    .icon = ICON_FA_ROTATE,
    .body = JE_STRUCT_DESC(RotationTweenComponent, "RotationTweenComponent", kRotationTweenFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(RotationTweenComponent),
    .codegenVar = "rotcomp",
};

const ComponentDesc kScaleTweenDesc{
    .type = ComponentType::ScaleTweenComponent, .cppName = "ScaleTweenComponent", .key = "scaletween", .displayName = "Scale Tween",
    .icon = ICON_FA_EXPAND,
    .body = JE_STRUCT_DESC(ScaleTweenComponent, "ScaleTweenComponent", kScaleTweenFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(ScaleTweenComponent),
    .codegenVar = "scalecomp",
};

const ComponentDesc kColorTweenDesc{
    .type = ComponentType::ColorTweenComponent, .cppName = "ColorTweenComponent", .key = "colortween", .displayName = "Color Tween",
    .icon = ICON_FA_PALETTE,
    .body = JE_STRUCT_DESC(ColorTweenComponent, "ColorTweenComponent", kColorTweenFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(ColorTweenComponent),
    .codegenVar = "colorcomp",
};

const ComponentDesc kAlphaTweenDesc{
    .type = ComponentType::AlphaTweenComponent, .cppName = "AlphaTweenComponent", .key = "alphatween", .displayName = "Alpha Tween",
    .icon = ICON_FA_EYE,
    .body = JE_STRUCT_DESC(AlphaTweenComponent, "AlphaTweenComponent", kAlphaTweenFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(AlphaTweenComponent),
    .codegenVar = "alphacomp",
};

const ComponentDesc kAnimationDesc{
    .type = ComponentType::AnimationComponent, .cppName = "AnimationComponent", .key = "animation", .displayName = "Animation", .icon = ICON_FA_FILM,
    .body = JE_STRUCT_DESC(AnimationComponent, "AnimationComponent", kAnimationFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(AnimationComponent),
    .codegenVar = "animcomp",
};

const ComponentDesc kBoneDesc{
    .type = ComponentType::BoneComponent, .cppName = "BoneComponent", .key = "bone", .displayName = "Bone", .icon = ICON_FA_BONE,
    .body = JE_STRUCT_DESC(BoneComponent, "BoneComponent", kBoneFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(BoneComponent),
    .decodeApply = DecodeApply::MarkOnly,
    .codegenVar = "bonecomp",
};

namespace {

    // the keys its codec reads
    constexpr const char* kSpriteAnimationKnownKeys[] = {"frames", "framesTime", "loop"};

}

const ComponentDesc kSpriteAnimationDesc{
    .type = ComponentType::SpriteAnimationComponent, .cppName = "SpriteAnimationComponent", .key = "spriteanimation",
    .displayName = "Sprite Animation", .icon = ICON_FA_FILM,
    .body = JE_STRUCT_DESC(SpriteAnimationComponent, "SpriteAnimationComponent", kSpriteAnimationFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(SpriteAnimationComponent),
    .codec = &kSpriteAnimationCodec,
    .codegen = JE_FACTORY_CODEGEN(createSpriteAnimationComponent),
    .extraKeys = kSpriteAnimationKnownKeys,
};

const ComponentDesc kKeyframeTracksDesc{
    .type = ComponentType::KeyframeTracksComponent, .cppName = "KeyframeTracksComponent", .key = "keyframetracks",
    .displayName = "Keyframe Tracks", .icon = ICON_FA_KEY,
    .body = JE_STRUCT_DESC(KeyframeTracksComponent, "KeyframeTracksComponent", kKeyframeTracksFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(KeyframeTracksComponent),
    .postDecode = &keyframeTracksPostDecode,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createKeyframeTracksComponent),
};

const ComponentDesc kTranslateTracksDesc{
    .type = ComponentType::TranslateTracksComponent, .cppName = "TranslateTracksComponent", .key = "translatetracks",
    .displayName = "Position Tracks", .icon = ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT,
    .body = JE_STRUCT_DESC(TranslateTracksComponent, "TranslateTracksComponent", kTranslateTracksFields),
    .hasEnabled = false, .structural = false, JE_COMPONENT_ACCESS(TranslateTracksComponent),
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createTranslateTracksComponent),
};

const ComponentDesc kRotateTracksDesc{
    .type = ComponentType::RotateTracksComponent, .cppName = "RotateTracksComponent", .key = "rotatetracks",
    .displayName = "Rotation Tracks", .icon = ICON_FA_CIRCLE_NOTCH,
    .body = JE_STRUCT_DESC(RotateTracksComponent, "RotateTracksComponent", kRotateTracksFields),
    .hasEnabled = false, .structural = false, JE_COMPONENT_ACCESS(RotateTracksComponent),
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createRotateTracksComponent),
};

const ComponentDesc kScaleTracksDesc{
    .type = ComponentType::ScaleTracksComponent, .cppName = "ScaleTracksComponent", .key = "scaletracks",
    .displayName = "Scale Tracks", .icon = ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER,
    .body = JE_STRUCT_DESC(ScaleTracksComponent, "ScaleTracksComponent", kScaleTracksFields),
    .hasEnabled = false, .structural = false, JE_COMPONENT_ACCESS(ScaleTracksComponent),
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createScaleTracksComponent),
};

namespace {

    // the keys its codec reads
    constexpr const char* kMorphTracksKnownKeys[] = {"inTangents", "outTangents", "values"};

}

const ComponentDesc kMorphTracksDesc{
    .type = ComponentType::MorphTracksComponent, .cppName = "MorphTracksComponent", .key = "morphtracks",
    .displayName = "Morph Tracks", .icon = ICON_FA_FACE_SMILE,
    .body = JE_STRUCT_DESC(MorphTracksComponent, "MorphTracksComponent", kMorphTracksFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(MorphTracksComponent),
    .codec = &kMorphTracksCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createMorphTracksComponent),
    .extraKeys = kMorphTracksKnownKeys,
};

}
