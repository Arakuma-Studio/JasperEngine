// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The players of authored animation (docs/specs/S08-reflection.md; animation clips spec 3.2,
// pose animation spec 4.1, vertex animation spec 3.3): the Pose Animator, the Animation Player and
// the Vertex Animator keep their own codecs (a missing key is the default, not the old value; a
// component decoded over keeps its runtime, the Vertex Animator only its Preview toggle); their
// tables give the properties, schema, Inspector metadata and the generic copy of a value.

#include "reflect/Components.h"

#include "Factory.h"
#include "Stream.h"
#include "component/AnimationPlayerComponent.h"
#include "component/PoseAnimatorComponent.h"
#include "component/VertexAnimatorComponent.h"

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
    constexpr TypeOps kLayerBlendOps = customProperty(ops::Enum<AnimationLayerBlend>);

    constexpr uint64_t kPlayerReload = UpdateFlags_AnimationPlayer_Reload;

    // clip: the path hint (asset-relative .anim or "<model>#<clip>"); guid: the .anim's (editor
    // only, empty for sub-clips); name: the state name (empty: the file stem or sub-clip name)
    constexpr FieldDesc kAnimationPlayerClipFields[] = {
        JE_FIELD(AnimationPlayerClip, clip, .updateFlags = kPlayerReload, .asset = AssetKind::AnimationClip, .assetForm = AssetForm::ClipEntry),
        JE_FIELD(AnimationPlayerClip, guid, .updateFlags = kPlayerReload, .label = "GUID"),
        JE_FIELD(AnimationPlayerClip, name, .updateFlags = kPlayerReload),
        JE_FIELD(AnimationPlayerClip, wrap, .ops = &kClipWrapOps, .enumDesc = &enums::kClipWrapMode, .updateFlags = kPlayerReload,
                 .tooltip = "Default: the clip's own."),
        // the index of its layer in the player's layers (out of range: the base layer)
        JE_FIELD(AnimationPlayerClip, layer, .flags = OmitIfDefault, .updateFlags = kPlayerReload, .tooltip = "The layer this state plays on."),
        // humanoid playback (upstream's anim-retarget, on S21's one retarget path): written when true
        JE_FIELD(AnimationPlayerClip, retarget, .flags = OmitIfDefault, .updateFlags = kPlayerReload,
                 .tooltip = "Play a generic clip retargeted from its source model."),
        JE_FIELD(AnimationPlayerClip, mirror, .flags = OmitIfDefault, .updateFlags = kPlayerReload,
                 .tooltip = "Humanoid playback: left and right swapped."),
        JE_FIELD(AnimationPlayerClip, footIK, .flags = OmitIfDefault, .updateFlags = kPlayerReload, .label = "Foot IK",
                 .tooltip = "Humanoid playback: the feet reach the clip's foot goals."),
    };

    // an authored layer (upstream anim layers spec 6.3, 7.1): layers[0] is the base layer (its
    // weight 1 and blend override whatever is written); mask: the .mask path hint (asset-relative),
    // maskGuid its GUID (editor only)
    constexpr FieldDesc kAnimationPlayerLayerFields[] = {
        JE_FIELD(AnimationPlayerLayer, name, .updateFlags = kPlayerReload),
        JE_FIELD(AnimationPlayerLayer, weight, .flags = Clamped | OmitIfDefault, .updateFlags = kPlayerReload, .min = 0.0f, .max = 1.0f,
                 .tooltip = "How much of the layer shows over the layers below (setLayerWeight changes it at runtime)."),
        JE_FIELD(AnimationPlayerLayer, blend, .ops = &kLayerBlendOps, .enumDesc = &enums::kAnimationLayerBlend, .flags = OmitIfDefault,
                 .updateFlags = kPlayerReload, .tooltip = "Override replaces the layers below; Additive adds the clip's change from its additive reference."),
        JE_FIELD(AnimationPlayerLayer, mask, .flags = OmitIfDefault, .updateFlags = kPlayerReload, .asset = AssetKind::AvatarMask,
                 .tooltip = "An Avatar Mask (.mask): which GameObjects the layer animates, and how much."),
        JE_FIELD(AnimationPlayerLayer, maskGuid, .flags = OmitIfDefault, .updateFlags = kPlayerReload, .label = "Mask GUID"),
    };

}

JE_REFLECT_STRUCT_TRAITS(AnimationPlayerClip, kAnimationPlayerClipStruct);
const StructDesc kAnimationPlayerClipStruct = JE_STRUCT_DESC(AnimationPlayerClip, "AnimationPlayerClip", kAnimationPlayerClipFields);
JE_REFLECT_STRUCT_TRAITS(AnimationPlayerLayer, kAnimationPlayerLayerStruct);
const StructDesc kAnimationPlayerLayerStruct = JE_STRUCT_DESC(AnimationPlayerLayer, "AnimationPlayerLayer", kAnimationPlayerLayerFields);

namespace {

    // enabled is the component checkbox; the clip list is `clips` (the whole list, what a prefab
    // override of the list holds) and clips[i].clip/guid/name/wrap; the runtime fields are no
    // properties. The settings are read by the player when it plays (no update flag).
    constexpr FieldDesc kAnimationPlayerFields[] = {
        JE_FIELD(AnimationPlayerComponent, enabled),
        JE_FIELD(AnimationPlayerComponent, clips, .flags = ListWhole | ListTemplate, .updateFlags = kPlayerReload),
        // empty: one Base Layer (anim::playerLayers); the whole list is what a prefab override holds
        JE_FIELD(AnimationPlayerComponent, layers, .flags = ListWhole | ListTemplate | OmitIfDefault, .updateFlags = kPlayerReload),
        JE_FIELD(AnimationPlayerComponent, defaultClip, .tooltip = "The state played automatically (empty: the first clip)."),
        JE_FIELD(AnimationPlayerComponent, playAutomatically),
        JE_FIELD(AnimationPlayerComponent, speed),
        JE_FIELD(AnimationPlayerComponent, defaultFadeTime, .unit = Unit::Seconds),
    };

    const ComponentCodec kAnimationPlayerCodec = JE_STREAM_CODEC(AnimationPlayerComponent, encodeAnimationPlayerComponent,
                                                                 decodeAnimationPlayerComponent);

    constexpr const char* kAnimationPlayerKnownKeys[] = {"controller", "clips", "defaultClip", "defaultFadeTime", "enabled", "layers", "playAutomatically", "speed"};

}

namespace {

    // ---------------------------------------------------------------- Vertex Animator

    constexpr uint64_t kVertexReload = UpdateFlags_VertexAnimator_Reload;

    // an attachment: the socket the GameObject `entity` follows (single entity mode)
    constexpr FieldDesc kVertexAnimatorAttachmentFields[] = {
        JE_FIELD(VertexAnimatorAttachment, socket),
        JE_FIELD(VertexAnimatorAttachment, entity, .ops = &ops::Entity),
    };

}

JE_REFLECT_STRUCT_TRAITS(VertexAnimatorAttachment, kVertexAnimatorAttachmentStruct);
const StructDesc kVertexAnimatorAttachmentStruct = JE_STRUCT_DESC(VertexAnimatorAttachment, "VertexAnimatorAttachment",
                                                                  kVertexAnimatorAttachmentFields);

namespace {

    // enabled is the component checkbox; `animation` is the .vat path hint (asset-relative) and
    // `guid` its GUID (editor); `attachments`, `instanceClips` and `instanceSpeeds` are whole lists
    // (what a prefab override holds; per-instance starts are Create Crowd's). The runtime fields
    // (Preview, the loaded asset, the GPU state) are no properties; the settings are read by the
    // system when it plays (no update flag)
    constexpr FieldDesc kVertexAnimatorFields[] = {
        JE_FIELD(VertexAnimatorComponent, enabled),
        JE_FIELD(VertexAnimatorComponent, animation, .updateFlags = kVertexReload, .asset = AssetKind::VertexAnimation),
        JE_FIELD(VertexAnimatorComponent, guid, .updateFlags = kVertexReload, .label = "GUID"),
        JE_FIELD(VertexAnimatorComponent, defaultClip, .tooltip = "The clip played automatically (empty: the first clip)."),
        JE_FIELD(VertexAnimatorComponent, playAutomatically),
        JE_FIELD(VertexAnimatorComponent, speed),
        JE_FIELD(VertexAnimatorComponent, randomStart, .tooltip = "A random start phase per instance (or GameObject)."),
        // kept >= 0 by its decode and updateEntity
        JE_FIELD(VertexAnimatorComponent, defaultFadeTime, .flags = Clamped, .updateFlags = kVertexReload, .unit = Unit::Seconds, .min = 0.0f),
        JE_FIELD(VertexAnimatorComponent, instanceEvents, .tooltip = "Events and finishes for every instance (cost per instance)."),
        JE_FIELD(VertexAnimatorComponent, attachments, .flags = ListWhole | ListTemplate),
        JE_FIELD(VertexAnimatorComponent, instanceClips, .flags = ListWhole, .updateFlags = kVertexReload),
        JE_FIELD(VertexAnimatorComponent, instanceSpeeds, .flags = ListWhole, .updateFlags = kVertexReload),
    };

    const ComponentCodec kVertexAnimatorCodec = JE_STREAM_CODEC(VertexAnimatorComponent, encodeVertexAnimatorComponent,
                                                                decodeVertexAnimatorComponent);

    constexpr const char* kVertexAnimatorKnownKeys[] = {"animation", "attachments", "defaultClip", "defaultFadeTime", "enabled", "guid",
                                                        "instanceClips", "instanceEvents", "instanceSpeeds", "playAutomatically",
                                                        "randomStart", "speed"};

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

const ComponentDesc kVertexAnimatorDesc{
    .type = ComponentType::VertexAnimatorComponent, .cppName = "VertexAnimatorComponent", .key = "vertexanimator",
    .displayName = "Vertex Animator", .icon = ICON_FA_PEOPLE_GROUP,
    .body = JE_STRUCT_DESC(VertexAnimatorComponent, "VertexAnimatorComponent", kVertexAnimatorFields),
    .hasEnabled = true, .structural = false, JE_COMPONENT_ACCESS(VertexAnimatorComponent),
    .codec = &kVertexAnimatorCodec,
    .decodeApply = DecodeApply::MarkOnly,
    .codegen = JE_FACTORY_CODEGEN(createVertexAnimatorComponent),
    .extraKeys = kVertexAnimatorKnownKeys,
};

}
