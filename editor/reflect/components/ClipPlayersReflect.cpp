// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The players of authored animation (docs/specs/S08-reflection.md; animation clips spec 3.2,
// pose animation spec 4.1): the Pose Animator and the Animation Player keep their own codecs (a
// missing key is the default, not the old value; a component decoded over keeps its runtime);
// their tables give the properties, schema, Inspector metadata and the generic copy of a value.

#include "reflect/Components.h"

#include "Factory.h"
#include "Stream.h"
#include "component/AnimationPlayerComponent.h"
#include "component/PoseAnimatorComponent.h"

namespace jasperengine::editor::reflect {

namespace {

    // ---------------------------------------------------------------- Pose Animator

    constexpr FieldDesc kPoseAnimatorFields[] = {
        JE_FIELD(PoseAnimatorComponent, animation, .updateFlags = UpdateFlags_PoseAnimator_Reload, .asset = AssetKind::PoseAnimation),
        JE_FIELD(PoseAnimatorComponent, autoplay),
        JE_FIELD(PoseAnimatorComponent, speed),
        // clamped to 0..1 by its decode and updateEntity
        JE_FIELD(PoseAnimatorComponent, weight, .flags = Clamped, .updateFlags = UpdateFlags_PoseAnimator_Reload, .min = 0.0f, .max = 1.0f,
                 .tooltip = "Blend over what the other animation wrote this frame."),
    };

    const ComponentCodec kPoseAnimatorCodec = JE_STREAM_CODEC(PoseAnimatorComponent, encodePoseAnimatorComponent, decodePoseAnimatorComponent);

    constexpr const char* kPoseAnimatorKnownKeys[] = {"animation", "autoplay", "speed", "weight"};

    // ---------------------------------------------------------------- Animation Player

    // a one-byte enum: a Custom property (PropertyCmd<ClipWrapMode>, Catalog::copyPropertyValue),
    // never read as an int-sized Enum
    constexpr TypeOps customProperty(TypeOps ops) {
        ops.propertyType = PropertyType::Custom;
        return ops;
    }

    constexpr TypeOps kClipWrapOps = customProperty(ops::Enum<ClipWrapMode>);

    constexpr uint64_t kPlayerReload = UpdateFlags_AnimationPlayer_Reload;

    // clip: the path hint (asset-relative .anim or "<model>#<clip>"); guid: the .anim's (editor
    // only, empty for sub-clips); name: the state name (empty: the file stem or sub-clip name)
    constexpr FieldDesc kAnimationPlayerClipFields[] = {
        JE_FIELD(AnimationPlayerClip, clip, .updateFlags = kPlayerReload, .asset = AssetKind::AnimationClip),
        JE_FIELD(AnimationPlayerClip, guid, .updateFlags = kPlayerReload, .label = "GUID"),
        JE_FIELD(AnimationPlayerClip, name, .updateFlags = kPlayerReload),
        JE_FIELD(AnimationPlayerClip, wrap, .ops = &kClipWrapOps, .enumDesc = &enums::kClipWrapMode, .updateFlags = kPlayerReload,
                 .tooltip = "Default: the clip's own."),
    };

}

JE_REFLECT_STRUCT_TRAITS(AnimationPlayerClip, kAnimationPlayerClipStruct);
const StructDesc kAnimationPlayerClipStruct = JE_STRUCT_DESC(AnimationPlayerClip, "AnimationPlayerClip", kAnimationPlayerClipFields);

namespace {

    // enabled is the component checkbox; the clip list is `clips` (the whole list, what a prefab
    // override of the list holds) and clips[i].clip/guid/name/wrap; the runtime fields are no
    // properties. The settings are read by the player when it plays (no update flag).
    constexpr FieldDesc kAnimationPlayerFields[] = {
        JE_FIELD(AnimationPlayerComponent, enabled),
        JE_FIELD(AnimationPlayerComponent, clips, .flags = ListWhole | ListTemplate, .updateFlags = kPlayerReload),
        JE_FIELD(AnimationPlayerComponent, defaultClip, .tooltip = "The state played automatically (empty: the first clip)."),
        JE_FIELD(AnimationPlayerComponent, playAutomatically),
        JE_FIELD(AnimationPlayerComponent, speed),
        JE_FIELD(AnimationPlayerComponent, defaultFadeTime, .unit = Unit::Seconds),
    };

    const ComponentCodec kAnimationPlayerCodec = JE_STREAM_CODEC(AnimationPlayerComponent, encodeAnimationPlayerComponent,
                                                                 decodeAnimationPlayerComponent);

    constexpr const char* kAnimationPlayerKnownKeys[] = {"clips", "defaultClip", "defaultFadeTime", "enabled", "playAutomatically", "speed"};

}

const ComponentDesc kPoseAnimatorDesc{
    .type = ComponentType::PoseAnimatorComponent, .cppName = "PoseAnimatorComponent", .key = "poseanimator",
    .displayName = "Pose Animator", .icon = ICON_FA_PERSON_RUNNING,
    .body = JE_STRUCT_DESC(PoseAnimatorComponent, "PoseAnimatorComponent", kPoseAnimatorFields),
    .hasEnabled = false, .structural = false, JE_COMPONENT_ACCESS(PoseAnimatorComponent),
    .codec = &kPoseAnimatorCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createPoseAnimatorComponent),
    .extraKeys = kPoseAnimatorKnownKeys,
};

const ComponentDesc kAnimationPlayerDesc{
    .type = ComponentType::AnimationPlayerComponent, .cppName = "AnimationPlayerComponent", .key = "animationplayer",
    .displayName = "Animation Player", .icon = ICON_FA_FILM,
    .body = JE_STRUCT_DESC(AnimationPlayerComponent, "AnimationPlayerComponent", kAnimationPlayerFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(AnimationPlayerComponent),
    .codec = &kAnimationPlayerCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createAnimationPlayerComponent),
    .extraKeys = kAnimationPlayerKnownKeys,
};

}
