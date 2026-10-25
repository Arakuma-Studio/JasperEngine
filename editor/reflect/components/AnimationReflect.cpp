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
        JE_FIELD(ActionComponent, target, .ops = &ops::Entity),
        JE_FIELD(ActionComponent, ownedTarget),
    };

    // a decoded action is not running: its timer and triggers start afresh
    void actionPostDecode(void* comp, const DecodeContext&) {
        ActionComponent& action = *static_cast<ActionComponent*>(comp);
        action.timecount = 0;
        action.startTrigger = false;
        action.stopTrigger = false;
        action.pauseTrigger = false;
    }

    // ---------------------------------------------------------------- Tweens

    constexpr FieldDesc kTweenFields[] = {
        JE_FIELD(TweenComponent, duration, .unit = Unit::Seconds, .min = 0.0f),
        JE_FIELD(TweenComponent, loop),
        JE_FIELD(TweenComponent, function, .label = "Ease"),
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

    // ---------------------------------------------------------------- tracks and sprite animation (their own codecs)

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

    constexpr FieldDesc kKeyframeTracksFields[] = {
        JE_FIELD(KeyframeTracksComponent, index, .flags = NoDefault),
        JE_FIELD(KeyframeTracksComponent, interpolation),
        JE_FIELD(KeyframeTracksComponent, times, .flags = kTrackList, .unit = Unit::Seconds),
        JE_FIELD(KeyframeTracksComponent, easings, .enumDesc = &enums::kEaseType, .flags = kTrackList),
    };

    // values, and the Hermite tangents of a GLTF CUBICSPLINE clip (kept beside them by key edits)
#define JE_TRACK_FIELDS(T) \
        JE_FIELD(T, values, .flags = kTrackList), \
        JE_FIELD(T, inTangents, .flags = ListWhole), \
        JE_FIELD(T, outTangents, .flags = ListWhole)

    constexpr FieldDesc kTranslateTracksFields[] = {JE_TRACK_FIELDS(TranslateTracksComponent)};
    constexpr FieldDesc kRotateTracksFields[] = {JE_TRACK_FIELDS(RotateTracksComponent)};
    constexpr FieldDesc kScaleTracksFields[] = {JE_TRACK_FIELDS(ScaleTracksComponent)};
    constexpr FieldDesc kMorphTracksFields[] = {JE_TRACK_FIELDS(MorphTracksComponent)};

#undef JE_TRACK_FIELDS

    const ComponentCodec kSpriteAnimationCodec = JE_STREAM_CODEC(SpriteAnimationComponent, encodeSpriteAnimationComponent, decodeSpriteAnimationComponent);
    const ComponentCodec kKeyframeTracksCodec = JE_STREAM_CODEC(KeyframeTracksComponent, encodeKeyframeTracksComponent, decodeKeyframeTracksComponent);
    const ComponentCodec kTranslateTracksCodec = JE_STREAM_CODEC(TranslateTracksComponent, encodeTranslateTracksComponent, decodeTranslateTracksComponent);
    const ComponentCodec kRotateTracksCodec = JE_STREAM_CODEC(RotateTracksComponent, encodeRotateTracksComponent, decodeRotateTracksComponent);
    const ComponentCodec kScaleTracksCodec = JE_STREAM_CODEC(ScaleTracksComponent, encodeScaleTracksComponent, decodeScaleTracksComponent);
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

namespace {

    // the keys its codec reads
    constexpr const char* kKeyframeTracksKnownKeys[] = {"easings", "index", "interpolation", "times"};

}

const ComponentDesc kKeyframeTracksDesc{
    .type = ComponentType::KeyframeTracksComponent, .cppName = "KeyframeTracksComponent", .key = "keyframetracks",
    .displayName = "Keyframe Tracks", .icon = ICON_FA_KEY,
    .body = JE_STRUCT_DESC(KeyframeTracksComponent, "KeyframeTracksComponent", kKeyframeTracksFields),
    .hasEnabled = false, .structural = true, JE_COMPONENT_ACCESS(KeyframeTracksComponent),
    .codec = &kKeyframeTracksCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createKeyframeTracksComponent),
    .extraKeys = kKeyframeTracksKnownKeys,
};

namespace {

    // the keys its codec reads
    constexpr const char* kTranslateTracksKnownKeys[] = {"inTangents", "outTangents", "values"};

}

const ComponentDesc kTranslateTracksDesc{
    .type = ComponentType::TranslateTracksComponent, .cppName = "TranslateTracksComponent", .key = "translatetracks",
    .displayName = "Position Tracks", .icon = ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT,
    .body = JE_STRUCT_DESC(TranslateTracksComponent, "TranslateTracksComponent", kTranslateTracksFields),
    .hasEnabled = false, .structural = false, JE_COMPONENT_ACCESS(TranslateTracksComponent),
    .codec = &kTranslateTracksCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createTranslateTracksComponent),
    .extraKeys = kTranslateTracksKnownKeys,
};

namespace {

    // the keys its codec reads
    constexpr const char* kRotateTracksKnownKeys[] = {"inTangents", "outTangents", "values"};

}

const ComponentDesc kRotateTracksDesc{
    .type = ComponentType::RotateTracksComponent, .cppName = "RotateTracksComponent", .key = "rotatetracks",
    .displayName = "Rotation Tracks", .icon = ICON_FA_CIRCLE_NOTCH,
    .body = JE_STRUCT_DESC(RotateTracksComponent, "RotateTracksComponent", kRotateTracksFields),
    .hasEnabled = false, .structural = false, JE_COMPONENT_ACCESS(RotateTracksComponent),
    .codec = &kRotateTracksCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createRotateTracksComponent),
    .extraKeys = kRotateTracksKnownKeys,
};

namespace {

    // the keys its codec reads
    constexpr const char* kScaleTracksKnownKeys[] = {"inTangents", "outTangents", "values"};

}

const ComponentDesc kScaleTracksDesc{
    .type = ComponentType::ScaleTracksComponent, .cppName = "ScaleTracksComponent", .key = "scaletracks",
    .displayName = "Scale Tracks", .icon = ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER,
    .body = JE_STRUCT_DESC(ScaleTracksComponent, "ScaleTracksComponent", kScaleTracksFields),
    .hasEnabled = false, .structural = false, JE_COMPONENT_ACCESS(ScaleTracksComponent),
    .codec = &kScaleTracksCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createScaleTracksComponent),
    .extraKeys = kScaleTracksKnownKeys,
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
